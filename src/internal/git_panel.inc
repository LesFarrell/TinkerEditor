/* Git branch caches, commands, and tab TreeView rendering/interaction. Git
   status collection and the shared OutputProcess runner stay in
   output_panel.inc; this component consumes that narrow host surface. */

/* ---- the Git panel's branch lists, refreshed asynchronously alongside
   status (see refresh_git_status()) ----
   render_git_panel() only ever reads these cached arrays: listing branches
   with run_git_sync(), as pick_git_branch() below does for its dialogs, is
   only safe from a plain menu/button WM_COMMAND, not from the panel's own
   paint/click handling, which can run nested inside another control's
   message processing (see g_gitPendingActionPath's comment, further down). */
static OutputProcess *g_gitBranchesProcess;
static int g_gitBranchesSession;
static char *g_gitBranchesOutput;
static size_t g_gitBranchesOutputLength;
static WCHAR **g_gitLocalBranches;
static int g_gitLocalBranchCount;
static int g_gitLocalBranchCapacity;
static WCHAR **g_gitRemoteBranchNames;
static int g_gitRemoteBranchCount;
static int g_gitRemoteBranchCapacity;
static WCHAR *g_gitBranchesWorkspace;

/* The Git panel's own header buttons (its window is created further down,
   in "the Git panel's own window"). */
static HWND g_hGitRefresh, g_hGitCommit, g_hGitStageAll, g_hGitClose;

static void clear_git_branch_cache(void) {
    free_branch_list(g_gitLocalBranches, g_gitLocalBranchCount);
    g_gitLocalBranches = NULL;
    g_gitLocalBranchCount = 0;
    g_gitLocalBranchCapacity = 0;
    free_branch_list(g_gitRemoteBranchNames, g_gitRemoteBranchCount);
    g_gitRemoteBranchNames = NULL;
    g_gitRemoteBranchCount = 0;
    g_gitRemoteBranchCapacity = 0;
}

static void shutdown_git_panel(void) {
    if (g_gitBranchesProcess) output_process_free(g_gitBranchesProcess);
    g_gitBranchesProcess = NULL;
    free(g_gitBranchesOutput);
    g_gitBranchesOutput = NULL;
    g_gitBranchesOutputLength = 0;
    free(g_gitBranchesWorkspace);
    g_gitBranchesWorkspace = NULL;
    clear_git_branch_cache();
}

/* Appends one for-each-ref line's short name (cursor + prefixLength, length
   bytes) to the names array, skipping it silently on allocation failure. */
static void add_branch_name(WCHAR ***names, int *count, int *capacity,
                            const char *cursor, size_t prefixLength, size_t length) {
    size_t nameLength = length - prefixLength;
    char *name = (char *)malloc(nameLength + 1);
    if (!name) return;
    memcpy(name, cursor + prefixLength, nameLength);
    name[nameLength] = 0;
    WCHAR *wide = to_wide(name);
    free(name);
    if (!wide) return;
    append_git_list_item(names, count, capacity, wide);
}

static void apply_git_branches_output(void) {
    clear_git_branch_cache();
    if (g_gitBranchesOutput) {
        static const char HEADS[] = "refs/heads/", REMOTES[] = "refs/remotes/";
        char *cursor = g_gitBranchesOutput;
        while (*cursor) {
            char *end = strchr(cursor, '\n');
            size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
            if (length && cursor[length - 1] == '\r') length--;
            if (length > sizeof(HEADS) - 1 && strncmp(cursor, HEADS, sizeof(HEADS) - 1) == 0) {
                add_branch_name(&g_gitLocalBranches, &g_gitLocalBranchCount,
                                &g_gitLocalBranchCapacity, cursor, sizeof(HEADS) - 1, length);
            } else if (length > sizeof(REMOTES) - 1 && strncmp(cursor, REMOTES, sizeof(REMOTES) - 1) == 0) {
                /* The remote's own symbolic HEAD pointer ("origin/HEAD")
                   isn't a real branch. */
                if (strncmp(cursor + length - 5, "/HEAD", 5) != 0)
                    add_branch_name(&g_gitRemoteBranchNames, &g_gitRemoteBranchCount,
                                    &g_gitRemoteBranchCapacity, cursor,
                                    sizeof(REMOTES) - 1, length);
            }
            if (!end) break;
            cursor = end + 1;
        }
    }
    render_git_panel();
}

static void handle_git_branches_chunk(OutputChunk *chunk) {
    if (g_gitBranchesProcess && chunk->session == g_gitBranchesSession) {
        if (chunk->length && g_gitBranchesOutputLength + chunk->length <= 1024 * 1024) {
            char *grown = (char *)realloc(g_gitBranchesOutput,
                g_gitBranchesOutputLength + chunk->length + 1);
            if (grown) {
                g_gitBranchesOutput = grown;
                memcpy(g_gitBranchesOutput + g_gitBranchesOutputLength, chunk->data, chunk->length);
                g_gitBranchesOutputLength += chunk->length;
                g_gitBranchesOutput[g_gitBranchesOutputLength] = 0;
            }
        }
        if (chunk->finished) {
            output_process_free(g_gitBranchesProcess);
            g_gitBranchesProcess = NULL;
            g_gitBranchesSession = 0;
            apply_git_branches_output();
        }
    }
    free(chunk);
}

static void refresh_git_branches(void) {
    if (!g_hMainWnd || !g_currentWorkspaceFolder) {
        if (g_gitBranchesProcess) output_process_free(g_gitBranchesProcess);
        g_gitBranchesProcess = NULL;
        free(g_gitBranchesOutput);
        g_gitBranchesOutput = NULL;
        g_gitBranchesOutputLength = 0;
        free(g_gitBranchesWorkspace);
        g_gitBranchesWorkspace = NULL;
        clear_git_branch_cache();
        render_git_panel();
        return;
    }
    int workspaceChanged = !g_gitBranchesWorkspace ||
                           _wcsicmp(g_gitBranchesWorkspace, g_currentWorkspaceFolder) != 0;
    if (workspaceChanged) {
        WCHAR *workspace = _wcsdup(g_currentWorkspaceFolder);
        free(g_gitBranchesWorkspace);
        g_gitBranchesWorkspace = workspace;
        clear_git_branch_cache();
        render_git_panel();
    }
    if (g_gitBranchesProcess) output_process_free(g_gitBranchesProcess);
    g_gitBranchesProcess = NULL;
    free(g_gitBranchesOutput);
    g_gitBranchesOutput = NULL;
    g_gitBranchesOutputLength = 0;
    int session = g_outputNextSession++;
    g_gitBranchesProcess = output_process_start(
        L"git.exe for-each-ref --format=%(refname) refs/heads refs/remotes",
        g_currentWorkspaceFolder, NULL, g_hMainWnd, WM_APP_GIT_BRANCHES, session);
    if (g_gitBranchesProcess) g_gitBranchesSession = session;
    else {
        clear_git_branch_cache();
        render_git_panel();
    }
}

/* Git's ref-name rules (see git-check-ref-format(1)) don't forbid a quote
   character, but every git_panel.inc command below splices a branch/remote/
   stash name straight into a "..." argument of the parameter string handed
   to CreateProcessW. A name containing a quote would close that argument
   early and let the rest of it be parsed as further command-line arguments,
   so names coming from git's own output (not already quote-checked, unlike
   the free-text commit/stash messages above) are rejected here first. */
static int reject_quoted_git_name(const WCHAR *name) {
    if (!name || !wcschr(name, L'"')) return 0;
    MessageBoxW(g_hMainWnd, L"That name contains a quotation mark and can't be used safely.",
                L"Git", MB_OK | MB_ICONWARNING);
    return 1;
}

/* Picks a branch with show_choice_dialog(); returns the chosen name (caller
   frees), or NULL if there were none to choose or the user cancelled.
   excludeCurrent leaves out the branch already checked out, for switch,
   merge and rebase targets. */
static WCHAR *pick_git_branch(const WCHAR *title, const WCHAR *prompt, const WCHAR *okLabel,
                              int excludeCurrent) {
    int count = 0;
    WCHAR **branches = git_list_branches(&count);
    if (!branches || count == 0) {
        MessageBoxW(g_hMainWnd, L"No local branches were found. Check that git.exe is on PATH.",
                    L"Git", MB_OK | MB_ICONWARNING);
        free_branch_list(branches, count);
        return NULL;
    }
    WCHAR **labels = (WCHAR **)malloc((size_t)count * sizeof(WCHAR *));
    int n = 0;
    if (labels)
        for (int i = 0; i < count; i++)
            if (!excludeCurrent || !g_gitBranchName || _wcsicmp(branches[i], g_gitBranchName) != 0)
                labels[n++] = branches[i];
    WCHAR *result = NULL;
    if (n > 0) {
        int chosen = show_choice_dialog(title, prompt, labels, n, okLabel);
        if (chosen >= 0) result = _wcsdup(labels[chosen]);
    } else {
        MessageBoxW(g_hMainWnd, L"There's no other branch to choose.", L"Git", MB_OK | MB_ICONINFORMATION);
    }
    free(labels);
    free_branch_list(branches, count);
    return result;
}

static void git_stage_file(const WCHAR *relativePath) {
    if (!g_currentWorkspaceFolder) return;
    size_t capacity = wcslen(relativePath) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"add -- \"%ls\"", relativePath);
    DWORD exitCode = 0;
    char *output = run_git_sync(parameters, g_currentWorkspaceFolder, &exitCode);
    free(parameters);
    report_git_sync_result(L"add", exitCode, output);
    free(output);
    refresh_git_status();
}

static void run_git_mutating_output(const WCHAR *parameters, const WCHAR *title);

static void git_unstage_file(const WCHAR *relativePath) {
    if (!g_currentWorkspaceFolder) return;
    size_t capacity = wcslen(relativePath) + 48;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, g_gitStatusUnborn ? L"rm -q --cached -- \"%ls\""
                                                     : L"reset -q HEAD -- \"%ls\"", relativePath);
    DWORD exitCode = 0;
    char *output = run_git_sync(parameters, g_currentWorkspaceFolder, &exitCode);
    free(parameters);
    report_git_sync_result(g_gitStatusUnborn ? L"rm --cached" : L"reset", exitCode, output);
    free(output);
    refresh_git_status();
}

static void git_discard_file(const WCHAR *relativePath, WCHAR mark) {
    if (!g_currentWorkspaceFolder) return;
    WCHAR prompt[MAX_PATH + 64];
    swprintf(prompt, MAX_PATH + 64, L"Discard changes to \"%ls\"? This cannot be undone.", relativePath);
    if (MessageBoxW(g_hMainWnd, prompt, L"Git", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    size_t capacity = wcslen(relativePath) + 48;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    /* An untracked file is removed with clean, not checkout - there's
       nothing in the index or HEAD for checkout to restore it from. */
    if (mark == L'?') swprintf(parameters, capacity, L"clean -fq -- \"%ls\"", relativePath);
    else swprintf(parameters, capacity, L"checkout -q -- \"%ls\"", relativePath);
    DWORD exitCode = 0;
    char *output = run_git_sync(parameters, g_currentWorkspaceFolder, &exitCode);
    free(parameters);
    report_git_sync_result(L"checkout", exitCode, output);
    free(output);
    refresh_git_status();
}

static void git_stage_all(void) {
    if (!g_currentWorkspaceFolder) return;
    DWORD exitCode = 0;
    char *output = run_git_sync(L"add -A", g_currentWorkspaceFolder, &exitCode);
    report_git_sync_result(L"add", exitCode, output);
    free(output);
    refresh_git_status();
}

static void git_unstage_all(void) {
    if (!g_currentWorkspaceFolder) return;
    DWORD exitCode = 0;
    char *output = run_git_sync(g_gitStatusUnborn ? L"rm -rq --cached -- ." : L"reset -q HEAD",
                                g_currentWorkspaceFolder, &exitCode);
    report_git_sync_result(g_gitStatusUnborn ? L"rm --cached" : L"reset", exitCode, output);
    free(output);
    refresh_git_status();
}

static void git_discard_all(void) {
    if (!g_currentWorkspaceFolder) return;
    if (g_gitStatusUnborn) {
        MessageBoxW(g_hMainWnd,
            L"This repository has no commit to restore. Discard All is unavailable until the first commit.",
            L"Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g_hMainWnd,
            L"Discard all staged and unstaged changes, and permanently remove untracked files? This cannot be undone.",
            L"Git", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    DWORD exitCode = 0;
    char *output = run_git_sync(L"reset --hard -q HEAD", g_currentWorkspaceFolder, &exitCode);
    if (!output) {
        MessageBoxW(g_hMainWnd, L"Git could not be started. Check that git.exe is on PATH.",
                    L"Git", MB_OK | MB_ICONWARNING);
        return;
    }
    report_git_sync_result(L"reset --hard", exitCode, output);
    free(output);
    if (exitCode != 0) { refresh_git_status(); return; }
    output = run_git_sync(L"clean -fdq", g_currentWorkspaceFolder, &exitCode);
    report_git_sync_result(L"clean", exitCode, output);
    free(output);
    refresh_git_status();
}

static void git_commit_prompt(void) {
    if (!g_currentWorkspaceFolder) {
        MessageBoxW(g_hMainWnd, L"Open a workspace folder to commit.", L"Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    WCHAR message[512] = L"";
    if (!show_text_input(L"Commit", L"Commit message:", L"", message, 512) || !message[0]) return;
    if (wcschr(message, L'"')) {
        MessageBoxW(g_hMainWnd, L"Commit messages can't contain a quotation mark.",
                    L"Git", MB_OK | MB_ICONWARNING);
        return;
    }
    size_t capacity = wcslen(message) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"commit -m \"%ls\"", message);
    /* Commit hooks and signing can take time or require user attention, so a
       commit belongs on the cancellable Run tab rather than in the modal
       quick-command runner. */
    run_git_mutating_output(parameters, L"Git Commit");
    free(parameters);
}

/* push/pull/fetch/merge/rebase/switch/branch run visibly on the Run tab,
   since they can be slow or need the user's attention; this flags the run so
   handle_output_chunk() refreshes Git status once it finishes. */
static void run_git_mutating_output(const WCHAR *parameters, const WCHAR *title) {
    g_gitRunPending = run_git_output(parameters, title) ? 1 : 0;
}

static WCHAR *pick_git_remote(void) {
    int count = 0;
    WCHAR **remotes = git_list_command_lines(L"remote", &count);
    if (!remotes || count == 0) {
        MessageBoxW(g_hMainWnd, L"No Git remotes are configured for this repository.",
                    L"Git", MB_OK | MB_ICONINFORMATION);
        free_branch_list(remotes, count);
        return NULL;
    }
    int chosen = count == 1 ? 0 :
        show_choice_dialog(L"Publish Branch", L"Publish to remote:", remotes, count, L"Publish");
    WCHAR *result = chosen >= 0 ? _wcsdup(remotes[chosen]) : NULL;
    free_branch_list(remotes, count);
    return result;
}

static void git_publish_branch(void) {
    if (!g_currentWorkspaceFolder || !g_gitBranchName || !g_gitBranchName[0]) {
        MessageBoxW(g_hMainWnd, L"Check out a local branch before publishing it.",
                    L"Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    WCHAR *remote = pick_git_remote();
    if (!remote) return;
    if (reject_quoted_git_name(remote) || reject_quoted_git_name(g_gitBranchName)) {
        free(remote);
        return;
    }
    size_t capacity = wcslen(remote) + wcslen(g_gitBranchName) + 48;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (parameters) {
        swprintf(parameters, capacity, L"push --set-upstream \"%ls\" \"%ls\"",
                 remote, g_gitBranchName);
        run_git_mutating_output(parameters, L"Git Publish Branch");
        free(parameters);
    }
    free(remote);
}

static void git_push(void) {
    if (!g_gitUpstreamName[0]) git_publish_branch();
    else run_git_mutating_output(L"push", L"Git Push");
}

static void git_pull(void) {
    run_git_mutating_output(L"pull", L"Git Pull");
}

static void git_fetch(void) {
    run_git_mutating_output(L"fetch --all", L"Git Fetch");
}

static WCHAR *pick_git_stash(const WCHAR *title, const WCHAR *okLabel) {
    int count = 0;
    WCHAR **labels = git_list_command_lines(L"stash list --format=%gd%x09%s", &count);
    if (!labels || count == 0) {
        MessageBoxW(g_hMainWnd, L"There are no stashes in this repository.",
                    L"Git", MB_OK | MB_ICONINFORMATION);
        free_branch_list(labels, count);
        return NULL;
    }
    int chosen = show_choice_dialog(title, L"Choose a stash:", labels, count, okLabel);
    WCHAR *result = NULL;
    if (chosen >= 0) {
        const WCHAR *tab = wcschr(labels[chosen], L'\t');
        size_t length = tab ? (size_t)(tab - labels[chosen]) : wcslen(labels[chosen]);
        result = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
        if (result) { memcpy(result, labels[chosen], length * sizeof(WCHAR)); result[length] = 0; }
    }
    free_branch_list(labels, count);
    return result;
}

static void git_stash_push_prompt(void) {
    if (!g_currentWorkspaceFolder) return;
    WCHAR message[512] = L"";
    if (!show_text_input(L"Stash Changes", L"Stash message (optional):", L"", message, 512)) return;
    if (wcschr(message, L'"')) {
        MessageBoxW(g_hMainWnd, L"Stash messages can't contain a quotation mark.",
                    L"Git", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!message[0]) {
        run_git_mutating_output(L"stash push -u", L"Git Stash Changes");
        return;
    }
    size_t capacity = wcslen(message) + 40;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"stash push -u -m \"%ls\"", message);
    run_git_mutating_output(parameters, L"Git Stash Changes");
    free(parameters);
}

static void git_stash_apply_prompt(void) {
    WCHAR *stash = pick_git_stash(L"Apply Stash", L"Apply");
    if (!stash) return;
    if (reject_quoted_git_name(stash)) { free(stash); return; }
    size_t capacity = wcslen(stash) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (parameters) {
        swprintf(parameters, capacity, L"stash apply \"%ls\"", stash);
        run_git_mutating_output(parameters, L"Git Apply Stash");
        free(parameters);
    }
    free(stash);
}

static void git_stash_pop(void) {
    run_git_mutating_output(L"stash pop", L"Git Pop Stash");
}

static void git_stash_drop_prompt(void) {
    WCHAR *stash = pick_git_stash(L"Drop Stash", L"Drop");
    if (!stash) return;
    if (reject_quoted_git_name(stash)) { free(stash); return; }
    WCHAR prompt[160];
    swprintf(prompt, 160, L"Permanently drop %ls?", stash);
    if (MessageBoxW(g_hMainWnd, prompt, L"Git", MB_YESNO | MB_ICONWARNING) == IDYES) {
        size_t capacity = wcslen(stash) + 32;
        WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
        if (parameters) {
            swprintf(parameters, capacity, L"stash drop \"%ls\"", stash);
            run_git_mutating_output(parameters, L"Git Drop Stash");
            free(parameters);
        }
    }
    free(stash);
}

static void git_switch_to_branch(const WCHAR *branch, int remote) {
    if (reject_quoted_git_name(branch)) return;
    size_t capacity = wcslen(branch) + 40;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, remote ? L"switch --track \"%ls\"" : L"switch \"%ls\"", branch);
    run_git_mutating_output(parameters, L"Git Switch Branch");
    free(parameters);
}

static void git_delete_branch(const WCHAR *branch) {
    if (reject_quoted_git_name(branch)) return;
    size_t promptCapacity = wcslen(branch) + 128;
    WCHAR *prompt = (WCHAR *)malloc(promptCapacity * sizeof(WCHAR));
    if (!prompt) return;
    swprintf(prompt, promptCapacity,
             L"Delete branch \"%ls\"? Git will keep it if it contains unmerged commits.", branch);
    int confirmed = MessageBoxW(g_hMainWnd, prompt, L"Git", MB_YESNO | MB_ICONWARNING) == IDYES;
    free(prompt);
    if (!confirmed) return;
    size_t capacity = wcslen(branch) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"branch -d \"%ls\"", branch);
    DWORD exitCode = 0;
    char *output = run_git_sync(parameters, g_currentWorkspaceFolder, &exitCode);
    if (!output) {
        MessageBoxW(g_hMainWnd, L"Git could not be started. Check that git.exe is on PATH.",
                    L"Git", MB_OK | MB_ICONWARNING);
        free(parameters);
        return;
    }
    if (exitCode != 0) {
        WCHAR *wide = output[0] ? to_wide(output) : NULL;
        size_t forceCapacity = wcslen(branch) + (wide ? wcslen(wide) : 0) + 160;
        WCHAR *forcePrompt = (WCHAR *)malloc(forceCapacity * sizeof(WCHAR));
        int force = 0;
        if (forcePrompt) {
            swprintf(forcePrompt, forceCapacity,
                L"Git could not safely delete \"%ls\":\n\n%ls\n\nForce delete it?",
                branch, wide ? wide : L"The branch may contain unmerged commits.");
            force = MessageBoxW(g_hMainWnd, forcePrompt, L"Git", MB_YESNO | MB_ICONWARNING) == IDYES;
        }
        free(forcePrompt);
        free(wide);
        if (force) {
            swprintf(parameters, capacity, L"branch -D \"%ls\"", branch);
            free(output);
            output = run_git_sync(parameters, g_currentWorkspaceFolder, &exitCode);
            report_git_sync_result(L"branch -D", exitCode, output);
        }
    }
    free(output);
    free(parameters);
    refresh_git_status();
}

static void git_switch_branch_prompt(void) {
    WCHAR *branch = pick_git_branch(L"Switch Branch", L"Switch to branch:", L"Switch", 1);
    if (!branch) return;
    git_switch_to_branch(branch, 0);
    free(branch);
}

static void git_new_branch_prompt(void) {
    if (!g_currentWorkspaceFolder) {
        MessageBoxW(g_hMainWnd, L"Open a workspace folder to create a branch.",
                    L"Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    WCHAR name[128] = L"";
    if (!show_text_input(L"New Branch", L"New branch name:", L"", name, 128) || !name[0]) return;
    if (reject_quoted_git_name(name)) return;
    size_t capacity = wcslen(name) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (parameters) {
        swprintf(parameters, capacity, L"switch -c \"%ls\"", name);
        run_git_mutating_output(parameters, L"Git New Branch");
        free(parameters);
    }
}

static void git_delete_branch_prompt(void) {
    WCHAR *branch = pick_git_branch(L"Delete Branch", L"Delete branch:", L"Delete", 1);
    if (!branch) return;
    git_delete_branch(branch);
    free(branch);
}

static void git_merge_prompt(void) {
    WCHAR *branch = pick_git_branch(L"Merge Into Current Branch", L"Merge branch:", L"Merge", 1);
    if (!branch) return;
    if (reject_quoted_git_name(branch)) { free(branch); return; }
    size_t capacity = wcslen(branch) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (parameters) {
        swprintf(parameters, capacity, L"merge \"%ls\"", branch);
        run_git_mutating_output(parameters, L"Git Merge");
        free(parameters);
    }
    free(branch);
}

static void git_rebase_prompt(void) {
    WCHAR *branch = pick_git_branch(L"Rebase Onto Branch", L"Rebase onto:", L"Rebase", 1);
    if (!branch) return;
    if (reject_quoted_git_name(branch)) { free(branch); return; }
    size_t capacity = wcslen(branch) + 32;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (parameters) {
        swprintf(parameters, capacity, L"rebase \"%ls\"", branch);
        run_git_mutating_output(parameters, L"Git Rebase");
        free(parameters);
    }
    free(branch);
}

static void git_merge_continue(void) {
    run_git_mutating_output(L"-c core.editor=true merge --continue", L"Git Continue Merge");
}

static void git_merge_abort(void) {
    if (MessageBoxW(g_hMainWnd, L"Abort the current merge and restore its pre-merge state?",
                    L"Git", MB_YESNO | MB_ICONWARNING) == IDYES)
        run_git_mutating_output(L"merge --abort", L"Git Abort Merge");
}

static void git_rebase_continue(void) {
    run_git_mutating_output(L"-c core.editor=true rebase --continue", L"Git Continue Rebase");
}

static void git_rebase_skip(void) {
    if (MessageBoxW(g_hMainWnd, L"Skip the current commit in this rebase?",
                    L"Git", MB_YESNO | MB_ICONWARNING) == IDYES)
        run_git_mutating_output(L"rebase --skip", L"Git Skip Rebase Commit");
}

static void git_rebase_abort(void) {
    if (MessageBoxW(g_hMainWnd, L"Abort the current rebase and restore its original branch state?",
                    L"Git", MB_YESNO | MB_ICONWARNING) == IDYES)
        run_git_mutating_output(L"rebase --abort", L"Git Abort Rebase");
}

/* show_git_panel()/hide_git_panel()/toggle_git_panel() are defined further
   down, alongside the rest of the panel's own window (create_git_panel()
   and friends). */
static void show_git_panel(void);

/* The status bar's Git segment (see StatusSubclassProc) reaches every branch
   command through this one compact menu. */
enum { GIT_MENU_PANEL = 1, GIT_MENU_SWITCH, GIT_MENU_NEW, GIT_MENU_DELETE,
      GIT_MENU_MERGE, GIT_MENU_REBASE, GIT_MENU_PUSH, GIT_MENU_PULL, GIT_MENU_FETCH };

static void show_git_branch_menu(HWND hwnd, POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, GIT_MENU_PANEL, L"Open Git Panel");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, GIT_MENU_SWITCH, L"Switch Branch...");
    AppendMenuW(menu, MF_STRING, GIT_MENU_NEW, L"New Branch...");
    AppendMenuW(menu, MF_STRING, GIT_MENU_DELETE, L"Delete Branch...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, GIT_MENU_MERGE, L"Merge Into Current...");
    AppendMenuW(menu, MF_STRING, GIT_MENU_REBASE, L"Rebase Onto...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, GIT_MENU_PUSH, L"Push");
    AppendMenuW(menu, MF_STRING, GIT_MENU_PULL, L"Pull");
    AppendMenuW(menu, MF_STRING, GIT_MENU_FETCH, L"Fetch");
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screenPoint.x, screenPoint.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    switch (command) {
        case GIT_MENU_PANEL: show_git_panel(); break;
        case GIT_MENU_SWITCH: git_switch_branch_prompt(); break;
        case GIT_MENU_NEW: git_new_branch_prompt(); break;
        case GIT_MENU_DELETE: git_delete_branch_prompt(); break;
        case GIT_MENU_MERGE: git_merge_prompt(); break;
        case GIT_MENU_REBASE: git_rebase_prompt(); break;
        case GIT_MENU_PUSH: git_push(); break;
        case GIT_MENU_PULL: git_pull(); break;
        case GIT_MENU_FETCH: git_fetch(); break;
    }
}

/* ---- the Git panel's own tab: a TreeView (checkboxes stage/unstage a file)
   instead of the output tabs' usual read-only Scintilla log. Each item's
   lParam is a GitTreeNode (see new_git_tree_node()), freed via
   TVN_DELETEITEMW - which fires for every item TVM_DELETEITEM(TVI_ROOT)
   removes, so rebuilding the tree on every refresh can't leak them. ---- */

typedef enum {
    GIT_TREE_HEADER, GIT_TREE_LOCAL_BRANCH, GIT_TREE_REMOTE_BRANCH,
    GIT_TREE_STAGED_FILE, GIT_TREE_UNSTAGED_FILE, GIT_TREE_CONFLICT_FILE
} GitTreeNodeKind;

typedef struct {
    GitTreeNodeKind kind;
    WCHAR *path; /* file path or branch name; NULL for header nodes */
    WCHAR mark;  /* '*' for the current local branch; a file's porcelain mark */
} GitTreeNode;

/* Both branch sections start collapsed - they sit below Staged/Changes now,
   so a long list would otherwise push those below the fold on first open. */
static int g_gitLocalBranchesCollapsed = 1;
static int g_gitRemoteBranchesCollapsed = 1;
/* Set while render_git_panel() is giving a file item its initial checkbox
   state, so handle_git_tree_item_changed() can tell that apart from the user
   actually clicking one - otherwise every rebuild would re-stage/unstage. */
static int g_buildingGitTree;

static GitTreeNode *new_git_tree_node(GitTreeNodeKind kind, const WCHAR *path, WCHAR mark) {
    GitTreeNode *node = (GitTreeNode *)calloc(1, sizeof(GitTreeNode));
    if (!node) return NULL;
    node->kind = kind;
    node->path = path ? _wcsdup(path) : NULL;
    node->mark = mark;
    return node;
}

static void handle_git_tree_delete_item(NMTREEVIEWW *info) {
    GitTreeNode *node = (GitTreeNode *)info->itemOld.lParam;
    if (node) { free(node->path); free(node); }
}

static GitTreeNode *git_tree_item_data(HWND tree, HTREEITEM item) {
    TVITEMW tv = {0};
    tv.mask = TVIF_PARAM | TVIF_HANDLE;
    tv.hItem = item;
    SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&tv);
    return (GitTreeNode *)tv.lParam;
}

/* Headers and branches get no checkbox at all (state image index 0); a file
   gets one, initially unchecked - set_git_tree_item_checked() below gives
   staged files their checked starting state. */
static HTREEITEM insert_git_tree_item(HWND tree, HTREEITEM parent, const WCHAR *text,
                                      GitTreeNode *node, int checkbox) {
    TVINSERTSTRUCTW insert = {0};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (WCHAR *)text;
    insert.item.lParam = (LPARAM)node;
    /* TVM_INSERTITEM/TVM_GETITEM have distinct ANSI/Unicode message codes and
       this project defines neither UNICODE nor _UNICODE (see file_explorer.c's
       tree_insert_item()), so this goes through the explicit *_W message
       rather than the ambiguous TreeView_InsertItem macro. */
    HTREEITEM item = (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&insert);
    if (item && !checkbox) {
        TVITEMW state = {0};
        state.mask = TVIF_HANDLE | TVIF_STATE;
        state.hItem = item;
        state.stateMask = TVIS_STATEIMAGEMASK;
        state.state = INDEXTOSTATEIMAGEMASK(0);
        SendMessageW(tree, TVM_SETITEMW, 0, (LPARAM)&state);
    }
    return item;
}

static void set_git_tree_item_checked(HWND tree, HTREEITEM item, int checked) {
    TVITEMW state = {0};
    state.mask = TVIF_HANDLE | TVIF_STATE;
    state.hItem = item;
    state.stateMask = TVIS_STATEIMAGEMASK;
    state.state = INDEXTOSTATEIMAGEMASK(checked ? 2 : 1);
    SendMessageW(tree, TVM_SETITEMW, 0, (LPARAM)&state);
}

/* The status word shown after a file's name, from git's own porcelain code
   (or '!' for a merge conflict, flagged internally - see the Conflicts
   section in render_git_panel()). */
static const WCHAR *git_status_word(WCHAR mark) {
    switch (mark) {
        case L'M': return L"Modified";
        case L'A': return L"Added";
        case L'D': return L"Deleted";
        case L'R': return L"Renamed";
        case L'C': return L"Copied";
        case L'?': return L"Untracked";
        case L'!': return L"Conflict";
        default: return L"Changed";
    }
}

static int git_status_file_is_conflict(const GitStatusFile *file) {
    return git_status_is_conflict((char)file->index, (char)file->worktree);
}

/* A Git path can exceed MAX_PATH. Build the display label to its actual
   length instead of truncating it through a fixed stack buffer. Caller frees. */
static WCHAR *git_file_label(const WCHAR *path, WCHAR mark) {
    const WCHAR *status = git_status_word(mark);
    size_t capacity = wcslen(path) + wcslen(status) + 5;
    WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (label) swprintf(label, capacity, L"%ls (%ls)", path, status);
    return label;
}

/* Reads only g_gitStatusFiles/g_gitBranchName and the branch cache above -
   never a subprocess - so it's always safe to call directly, including from
   a checkbox click (see handle_git_tree_item_changed()). */
static void render_git_panel(void) {
    HWND tree = g_hGitTree;
    if (!tree) return;
    g_buildingGitTree = 1;
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree); /* frees every GitTreeNode via TVN_DELETEITEMW */
    if (!g_currentWorkspaceFolder) {
        insert_git_tree_item(tree, TVI_ROOT, L"Open a workspace folder to use Git.",
                             new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree, NULL, TRUE);
        g_buildingGitTree = 0;
        return;
    }
    if (g_gitStatusAvailable < 0) {
        insert_git_tree_item(tree, TVI_ROOT,
            L"Git status unavailable. Check the workspace and git.exe.",
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree, NULL, TRUE);
        g_buildingGitTree = 0;
        return;
    }
    if (g_gitStatusAvailable == 0) {
        insert_git_tree_item(tree, TVI_ROOT, L"Refreshing Git status...",
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree, NULL, TRUE);
        g_buildingGitTree = 0;
        return;
    }

    int stagedCount = 0, unstagedCount = 0, untrackedCount = 0, conflictCount = 0;
    for (int i = 0; i < g_gitStatusFileCount; i++) {
        GitStatusFile *file = &g_gitStatusFiles[i];
        int conflict = git_status_file_is_conflict(file);
        if (conflict) conflictCount++;
        else if (file->index == L'?') untrackedCount++;
        else {
            if (file->index != L' ') stagedCount++;
            if (file->worktree != L' ') unstagedCount++;
        }
    }

    if (stagedCount) {
        WCHAR label[48];
        swprintf(label, 48, L"Staged Changes (%d)", stagedCount);
        HTREEITEM header = insert_git_tree_item(tree, TVI_ROOT, label,
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        for (int i = 0; i < g_gitStatusFileCount; i++) {
            GitStatusFile *file = &g_gitStatusFiles[i];
            if (!git_status_file_is_conflict(file) &&
                file->index != L' ' && file->index != L'?') {
                WCHAR *text = git_file_label(file->path, file->index);
                HTREEITEM item = insert_git_tree_item(tree, header, text ? text : file->path,
                    new_git_tree_node(GIT_TREE_STAGED_FILE, file->path, file->index), 1);
                free(text);
                set_git_tree_item_checked(tree, item, 1);
            }
        }
        TreeView_Expand(tree, header, TVE_EXPAND);
    }
    if (unstagedCount || untrackedCount) {
        WCHAR label[48];
        swprintf(label, 48, L"Changes (%d)", unstagedCount + untrackedCount);
        HTREEITEM header = insert_git_tree_item(tree, TVI_ROOT, label,
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        for (int i = 0; i < g_gitStatusFileCount; i++) {
            GitStatusFile *file = &g_gitStatusFiles[i];
            if (!git_status_file_is_conflict(file) && file->worktree != L' ') {
                WCHAR *text = git_file_label(file->path, file->worktree);
                HTREEITEM item = insert_git_tree_item(tree, header, text ? text : file->path,
                    new_git_tree_node(GIT_TREE_UNSTAGED_FILE, file->path, file->worktree), 1);
                free(text);
                set_git_tree_item_checked(tree, item, 0);
            }
        }
        TreeView_Expand(tree, header, TVE_EXPAND);
    }
    if (conflictCount) {
        WCHAR label[48];
        swprintf(label, 48, L"Conflicts (%d)", conflictCount);
        HTREEITEM header = insert_git_tree_item(tree, TVI_ROOT, label,
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        for (int i = 0; i < g_gitStatusFileCount; i++) {
            GitStatusFile *file = &g_gitStatusFiles[i];
            int conflict = git_status_file_is_conflict(file);
            if (conflict) {
                WCHAR *text = git_file_label(file->path, L'!');
                HTREEITEM item = insert_git_tree_item(tree, header, text ? text : file->path,
                    new_git_tree_node(GIT_TREE_CONFLICT_FILE, file->path, L'!'), 1);
                free(text);
                set_git_tree_item_checked(tree, item, 0);
            }
        }
        TreeView_Expand(tree, header, TVE_EXPAND);
    }
    if (!stagedCount && !unstagedCount && !untrackedCount && !conflictCount)
        insert_git_tree_item(tree, TVI_ROOT, L"No changes.",
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);

    if (g_gitLocalBranchCount > 0) {
        const WCHAR *currentName = g_gitBranchName && g_gitBranchName[0] ? g_gitBranchName : L"none";
        size_t labelCapacity = wcslen(currentName) + 24;
        WCHAR *label = (WCHAR *)malloc(labelCapacity * sizeof(WCHAR));
        if (label) swprintf(label, labelCapacity, L"Local Branches (%ls)", currentName);
        HTREEITEM header = insert_git_tree_item(tree, TVI_ROOT,
            label ? label : L"Local Branches",
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        free(label);
        for (int i = 0; i < g_gitLocalBranchCount; i++) {
            int current = g_gitBranchName && _wcsicmp(g_gitLocalBranches[i], g_gitBranchName) == 0;
            size_t textCapacity = wcslen(g_gitLocalBranches[i]) + 12;
            WCHAR *text = (WCHAR *)malloc(textCapacity * sizeof(WCHAR));
            if (text) swprintf(text, textCapacity, current ? L"%ls (current)" : L"%ls",
                               g_gitLocalBranches[i]);
            insert_git_tree_item(tree, header, text ? text : g_gitLocalBranches[i],
                new_git_tree_node(GIT_TREE_LOCAL_BRANCH, g_gitLocalBranches[i], current ? L'*' : L' '), 0);
            free(text);
        }
        if (!g_gitLocalBranchesCollapsed) TreeView_Expand(tree, header, TVE_EXPAND);
    }

    if (g_gitRemoteBranchCount > 0) {
        HTREEITEM header = insert_git_tree_item(tree, TVI_ROOT, L"Remote Branches",
            new_git_tree_node(GIT_TREE_HEADER, NULL, 0), 0);
        for (int i = 0; i < g_gitRemoteBranchCount; i++)
            insert_git_tree_item(tree, header, g_gitRemoteBranchNames[i],
                new_git_tree_node(GIT_TREE_REMOTE_BRANCH, g_gitRemoteBranchNames[i], 0), 0);
        if (!g_gitRemoteBranchesCollapsed) TreeView_Expand(tree, header, TVE_EXPAND);
    }

    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree, NULL, TRUE);
    g_buildingGitTree = 0;
}

static HTREEITEM git_tree_item_at_message_pos(HWND tree, POINT *screenOut, UINT *flagsOut) {
    DWORD pos = GetMessagePos();
    POINT screen = { GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    if (screenOut) *screenOut = screen;
    TVHITTESTINFO hit = {0};
    hit.pt = screen;
    ScreenToClient(tree, &hit.pt);
    HTREEITEM item = TreeView_HitTest(tree, &hit);
    if (flagsOut) *flagsOut = hit.flags;
    return item;
}

/* Every git action the tree can trigger (checkbox click, double-click, or
   its right-click menu) is deferred through here rather than called
   directly. All three arrive nested inside the TreeView's own click
   processing - a checkbox toggle and a double-click as NM_* notifications
   fired from within its WM_LBUTTONDOWN/UP handling, and a context-menu
   choice still on that same stack after TrackPopupMenu() returns (that call
   runs its own modal loop, but doesn't unwind the frame that opened it).
   Calling run_git_sync() (its own nested message loop) or run_git_output()/
   git_switch_to_branch() (which switch tabs and hide/show panes) while
   still nested like that corrupts the UI - the git panel's own double-click
   diff doing nothing but switch to an empty Output tab was exactly this.
   So nothing here runs until the posted message below is handled back at
   the top of the message loop. */
static WCHAR *g_gitPendingActionPath;
static WCHAR g_gitPendingActionMark;
static int g_gitPendingAction; /* 0 none, 1 stage, 2 unstage, 3 diff, 4 switch local, 5 discard, 6 delete, 7 staged diff, 8 switch remote, 9 untracked diff, 10 log, 11 blame */

static void queue_git_panel_action(int action, const WCHAR *path, WCHAR mark) {
    WCHAR *copy = _wcsdup(path);
    if (!copy) return;
    free(g_gitPendingActionPath);
    g_gitPendingActionPath = copy;
    g_gitPendingActionMark = mark;
    g_gitPendingAction = action;
    PostMessageW(g_hMainWnd, WM_APP_GIT_PANEL_ACTION, 0, 0);
}

static void run_pending_git_panel_action(void) {
    WCHAR *path = g_gitPendingActionPath;
    WCHAR mark = g_gitPendingActionMark;
    int action = g_gitPendingAction;
    g_gitPendingActionPath = NULL;
    g_gitPendingAction = 0;
    if (!path) return;
    switch (action) {
        case 2: git_unstage_file(path); break;
        case 3: git_diff_panel_file(path, GIT_DIFF_UNSTAGED); break;
        case 4: git_switch_to_branch(path, 0); break;
        case 5: git_discard_file(path, mark); break;
        case 6: git_delete_branch(path); break;
        case 7: git_diff_panel_file(path, GIT_DIFF_STAGED); break;
        case 8: git_switch_to_branch(path, 1); break;
        case 9: git_diff_panel_file(path, GIT_DIFF_UNTRACKED); break;
        case 10: git_log_panel_file(path); break;
        case 11: git_blame_panel_file(path); break;
        default: git_stage_file(path); break; /* also how a conflict's resolution is marked */
    }
    free(path);
}

static void handle_git_tree_double_click(HWND tree) {
    UINT flags = 0;
    HTREEITEM item = git_tree_item_at_message_pos(tree, NULL, &flags);
    /* A click on the checkbox or the expand/collapse glyph already did its
       own thing through the TreeView's default handling; this is only for
       a double-click on the row's icon/label. */
    if (!item || (flags & (TVHT_ONITEMSTATEICON | TVHT_ONITEMBUTTON))) return;
    GitTreeNode *node = git_tree_item_data(tree, item);
    if (!node) return;
    if (node->kind == GIT_TREE_LOCAL_BRANCH) {
        if (node->mark != L'*') queue_git_panel_action(4, node->path, 0);
    } else if (node->kind == GIT_TREE_REMOTE_BRANCH) {
        queue_git_panel_action(8, node->path, 0);
    } else if (node->kind == GIT_TREE_STAGED_FILE) {
        queue_git_panel_action(7, node->path, 0);
    } else if (node->kind == GIT_TREE_UNSTAGED_FILE || node->kind == GIT_TREE_CONFLICT_FILE) {
        queue_git_panel_action(node->mark == L'?' ? 9 : 3, node->path, 0);
    }
}

static void handle_git_tree_item_changed(NMTVITEMCHANGE *change) {
    if (g_buildingGitTree) return;
    int newChecked = ((change->uStateNew & TVIS_STATEIMAGEMASK) >> 12) == 2;
    int oldChecked = ((change->uStateOld & TVIS_STATEIMAGEMASK) >> 12) == 2;
    if (newChecked == oldChecked) return;
    GitTreeNode *node = git_tree_item_data(g_hGitTree, change->hItem);
    if (!node) return;
    if (node->kind == GIT_TREE_STAGED_FILE && !newChecked) queue_git_panel_action(2, node->path, 0);
    else if ((node->kind == GIT_TREE_UNSTAGED_FILE || node->kind == GIT_TREE_CONFLICT_FILE) && newChecked)
        queue_git_panel_action(1, node->path, 0);
}

enum { GIT_CTX_STAGE = 1, GIT_CTX_UNSTAGE, GIT_CTX_DISCARD, GIT_CTX_DIFF, GIT_CTX_SWITCH, GIT_CTX_DELETE_BRANCH,
       GIT_CTX_LOG, GIT_CTX_BLAME };

static void show_git_tree_context_menu(HWND tree, HTREEITEM item, POINT screenPoint) {
    if (!item) return;
    TreeView_SelectItem(tree, item);
    GitTreeNode *node = git_tree_item_data(tree, item);
    if (!node) return;
    /* TrackPopupMenu pumps messages. A status result can therefore rebuild
       the tree and free node while the menu is open, so retain only an owned
       snapshot across that modal loop. */
    GitTreeNodeKind kind = node->kind;
    WCHAR mark = node->mark;
    WCHAR *path = node->path ? _wcsdup(node->path) : NULL;
    if (node->path && !path) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) { free(path); return; }
    if (kind == GIT_TREE_REMOTE_BRANCH) {
        AppendMenuW(menu, MF_STRING, GIT_CTX_SWITCH, L"Switch to This Branch");
    } else if (kind == GIT_TREE_LOCAL_BRANCH) {
        if (mark != L'*') {
            AppendMenuW(menu, MF_STRING, GIT_CTX_SWITCH, L"Switch to This Branch");
            AppendMenuW(menu, MF_STRING, GIT_CTX_DELETE_BRANCH, L"Delete Branch...");
        }
    } else if (kind == GIT_TREE_STAGED_FILE) {
        AppendMenuW(menu, MF_STRING, GIT_CTX_UNSTAGE, L"Unstage");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, GIT_CTX_DIFF, L"Open Diff");
        if (mark != L'?') {
            AppendMenuW(menu, MF_STRING, GIT_CTX_LOG, L"Show File History");
            AppendMenuW(menu, MF_STRING, GIT_CTX_BLAME, L"Blame File");
        }
    } else if (kind == GIT_TREE_UNSTAGED_FILE || kind == GIT_TREE_CONFLICT_FILE) {
        AppendMenuW(menu, MF_STRING, GIT_CTX_STAGE,
                   kind == GIT_TREE_CONFLICT_FILE ? L"Mark Resolved" : L"Stage");
        AppendMenuW(menu, MF_STRING, GIT_CTX_DISCARD, L"Discard Changes...");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, GIT_CTX_DIFF, L"Open Diff");
        if (mark != L'?') {
            AppendMenuW(menu, MF_STRING, GIT_CTX_LOG, L"Show File History");
            AppendMenuW(menu, MF_STRING, GIT_CTX_BLAME, L"Blame File");
        }
    }
    UINT command = GetMenuItemCount(menu) > 0
        ? TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                         screenPoint.x, screenPoint.y, 0, g_hMainWnd, NULL)
        : 0;
    DestroyMenu(menu);
    switch (command) {
        case GIT_CTX_STAGE: queue_git_panel_action(1, path, 0); break;
        case GIT_CTX_UNSTAGE: queue_git_panel_action(2, path, 0); break;
        case GIT_CTX_DISCARD: queue_git_panel_action(5, path, mark); break;
        case GIT_CTX_DIFF:
            queue_git_panel_action(kind == GIT_TREE_STAGED_FILE ? 7 :
                                   mark == L'?' ? 9 : 3, path, 0);
            break;
        case GIT_CTX_SWITCH:
            queue_git_panel_action(kind == GIT_TREE_REMOTE_BRANCH ? 8 : 4, path, 0);
            break;
        case GIT_CTX_DELETE_BRANCH: queue_git_panel_action(6, path, 0); break;
        case GIT_CTX_LOG: queue_git_panel_action(10, path, 0); break;
        case GIT_CTX_BLAME: queue_git_panel_action(11, path, 0); break;
    }
    free(path);
}

static void handle_git_tree_mouse_context_menu(HWND tree) {
    POINT screenPt;
    HTREEITEM item = git_tree_item_at_message_pos(tree, &screenPt, NULL);
    show_git_tree_context_menu(tree, item, screenPt);
}

static void handle_git_tree_keyboard_context_menu(HWND tree) {
    HTREEITEM item = TreeView_GetSelection(tree);
    RECT itemRect = {0};
    POINT screenPt = {0};
    if (item && TreeView_GetItemRect(tree, item, &itemRect, TRUE)) {
        screenPt.x = itemRect.left;
        screenPt.y = itemRect.bottom;
    } else {
        RECT client;
        GetClientRect(tree, &client);
        screenPt.x = client.left + 12;
        screenPt.y = client.top + 12;
    }
    ClientToScreen(tree, &screenPt);
    show_git_tree_context_menu(tree, item, screenPt);
}

/* The Git tree's only subclass job: the context-menu key (WM_CONTEXTMENU
   with lParam -1, i.e. no cursor position) needs its own handling, since
   NM_RCLICK (handled in OutputPanelSubclassProc's WM_NOTIFY) only covers a
   mouse right-click. */
static LRESULT CALLBACK GitTreeSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data; (void)wParam;
    if (msg == WM_CONTEXTMENU && (LONG)lParam == -1) {
        handle_git_tree_keyboard_context_menu(hwnd);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, GitTreeSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* Forces every item's text to the app's own theme colour rather than
   whatever the "DarkMode_Explorer"/"Explorer" visual style would otherwise
   pick per item (observed: it colours some leaf items differently from
   others with no code-visible cause - deterministic colouring sidesteps
   that instead of chasing it). The current local branch and section headers
   get the accent colour, so they still read as distinct from a plain file
   or branch name. */
static LRESULT git_tree_customdraw(NMTVCUSTOMDRAW *draw) {
    switch (draw->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            GitTreeNode *node = (GitTreeNode *)draw->nmcd.lItemlParam;
            int accent = node && (node->kind == GIT_TREE_HEADER ||
                                  (node->kind == GIT_TREE_LOCAL_BRANCH && node->mark == L'*'));
            draw->clrText = accent ? theme_accent_color() : theme_fg_color();
            return CDRF_DODEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

/* ---- the Git panel's own window ----
   Unlike the other output tabs, Git has its own independently-dockable
   panel (View > Git Panel / View > Git Panel on Right), the same shape as
   the file explorer: a small header of buttons above the tree, sized and
   positioned by layout_git_panel() below rather than as one more tab inside
   output_panel.inc's shared host. */

static void size_git_panel(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int dpi = GetDpiForWindow(panel);
    int pad = MulDiv(OUTPUT_PANEL_PADDING, dpi, 96);
    int header = MulDiv(OUTPUT_HEADER_HEIGHT, dpi, 96);
    int row = header - pad;
    int closeWidth = MulDiv(DEBUG_CLOSE_BUTTON_WIDTH, dpi, 96);
    int buttonWidth = MulDiv(OUTPUT_BUTTON_WIDTH, dpi, 96);
    HWND buttons[] = { g_hGitClose, g_hGitCommit, g_hGitStageAll, g_hGitRefresh };
    int widths[] = { closeWidth, buttonWidth, buttonWidth, buttonWidth };
    int x = rc.right;
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        x -= pad + widths[i];
        MoveWindow(buttons[i], x, pad / 2, widths[i], row, TRUE);
        InvalidateRect(buttons[i], NULL, TRUE);
    }
    RECT content = { pad, header, rc.right - pad, rc.bottom - pad };
    if (content.right < content.left) content.right = content.left;
    if (content.bottom < content.top) content.bottom = content.top;
    MoveWindow(g_hGitTree, content.left + 1, content.top + 1,
              content.right - content.left - 2, content.bottom - content.top - 2, TRUE);
}

static void apply_git_panel_theme(void) {
    if (!g_hGitTree) return;
    apply_explorer_theme(g_hGitTree);
    TreeView_SetBkColor(g_hGitTree, theme_bg_color());
    TreeView_SetTextColor(g_hGitTree, theme_fg_color());
    themed_scrollbar_refresh(g_hGitTree);
    InvalidateRect(g_hGitTree, NULL, TRUE);
    if (g_hGitPanel) InvalidateRect(g_hGitPanel, NULL, TRUE);
    HWND buttons[] = { g_hGitRefresh, g_hGitStageAll, g_hGitCommit, g_hGitClose };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (buttons[i]) InvalidateRect(buttons[i], NULL, TRUE);
}

static void paint_git_panel(HWND panel, HDC hdc) {
    RECT rc;
    GetClientRect(panel, &rc);
    fill_rect_color(hdc, &rc, theme_bg_color());
    int dpi = GetDpiForWindow(panel);
    int pad = MulDiv(OUTPUT_PANEL_PADDING, dpi, 96);
    int header = MulDiv(OUTPUT_HEADER_HEIGHT, dpi, 96);
    RECT content = { pad, header, rc.right - pad, rc.bottom - pad };
    frame_rect(hdc, &content, theme_border_color());
}

static void hide_git_panel(void);

static LRESULT CALLBACK GitPanelSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    switch (msg) {
        case WM_SIZE:
            size_git_panel(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORBTN:
            if (g_mainBkBrush) return (LRESULT)g_mainBkBrush;
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint_git_panel(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_git_panel(hwnd, (HDC)wParam);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_GIT_REFRESH: refresh_git_status(); return 0;
                case ID_GIT_STAGE_ALL: git_stage_all(); return 0;
                case ID_GIT_COMMIT: git_commit_prompt(); return 0;
                case ID_GIT_CLOSE: hide_git_panel(); return 0;
            }
            break;
        case WM_NOTIFY: {
            NMHDR *notification = (NMHDR *)lParam;
            if (notification->hwndFrom != g_hGitTree) break;
            if (notification->code == NM_CUSTOMDRAW)
                return git_tree_customdraw((NMTVCUSTOMDRAW *)lParam);
            if (notification->code == TVN_DELETEITEMW)
                handle_git_tree_delete_item((NMTREEVIEWW *)lParam);
            else if (notification->code == TVN_ITEMCHANGEDW)
                handle_git_tree_item_changed((NMTVITEMCHANGE *)lParam);
            else if (notification->code == NM_DBLCLK)
                handle_git_tree_double_click(g_hGitTree);
            else if (notification->code == NM_RCLICK)
                handle_git_tree_mouse_context_menu(g_hGitTree);
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_debug_panel_button(dis); return TRUE; }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (g_mainBkBrush) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, theme_fg_color());
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_mainBkBrush;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, GitPanelSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void create_git_panel(HWND hwnd) {
    g_hGitPanel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_GIT_PANEL,
                                  GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hGitPanel, GitPanelSubclassProc, 1, 0);
    /* No WS_VISIBLE here, same as the file explorer's own tree: whether it's
       actually shown follows the panel's own visibility, reasserted every
       layout_git_panel() call below rather than fixed at creation time. */
    g_hGitTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VSCROLL | WS_TABSTOP |
            TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
            TVS_DISABLEDRAGDROP | TVS_CHECKBOXES,
        0, 0, 0, 0, g_hGitPanel, (HMENU)(UINT_PTR)ID_GIT_TREE, GetModuleHandleW(NULL), NULL);
    TreeView_SetExtendedStyle(g_hGitTree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    TreeView_SetItemHeight(g_hGitTree, MulDiv(18, GetDpiForWindow(hwnd), 96));
    SetWindowSubclass(g_hGitTree, GitTreeSubclassProc, 1, 0);
    themed_scrollbar_attach(g_hGitTree, THEMED_SCROLL_TREEVIEW);
    g_hGitRefresh = create_output_button(g_hGitPanel, L"Refresh", ID_GIT_REFRESH);
    g_hGitStageAll = create_output_button(g_hGitPanel, L"Stage All", ID_GIT_STAGE_ALL);
    g_hGitCommit = create_output_button(g_hGitPanel, L"Commit...", ID_GIT_COMMIT);
    g_hGitClose = create_output_button(g_hGitPanel, L"X", ID_GIT_CLOSE);
    ShowWindow(g_hGitPanel, g_gitPanelVisible ? SW_SHOW : SW_HIDE);
    /* Applied here, not left to apply_output_panel_theme()'s call into
       apply_git_panel_theme(): create_output_panel() (which makes that call)
       runs before create_git_panel(), so g_hGitTree wouldn't exist yet and
       the panel would start in the system's default colours until the next
       theme change. Mirrors explorer_init()'s own explorer_apply_theme()
       call in file_explorer.c for the same reason. */
    apply_git_panel_theme();
}

/* The Git panel docks at the left or right edge, exactly like the file
   explorer (see layout_explorer() in ui_shell.inc), carved out of whatever
   area{} is left once the explorer has taken its own slice. */
static void layout_git_panel(HWND hwnd, LayoutArea *area) {
    if (!g_hGitPanel || !g_gitPanelVisible || g_focusMode) {
        g_gitPanelSplitterRect = (RECT){0, 0, 0, 0};
        if (g_hGitPanel) ShowWindow(g_hGitPanel, SW_HIDE);
        return;
    }
    int dpi = GetDpiForWindow(hwnd);
    if (g_gitPanelWidth < 0)
        g_gitPanelWidth = MulDiv(GIT_PANEL_WIDTH, dpi, 96);
    int minWidth = MulDiv(GIT_PANEL_MIN_WIDTH, dpi, 96);
    int maxWidth = area->width * 2 / 3;
    if (g_gitPanelWidth < minWidth) g_gitPanelWidth = minWidth;
    if (maxWidth > minWidth && g_gitPanelWidth > maxWidth) g_gitPanelWidth = maxWidth;
    if (g_gitPanelOnRight) {
        int panelX = area->x + area->width - g_gitPanelWidth;
        g_gitPanelSplitterRect = (RECT){ panelX - SPLITTER_WIDTH, area->y,
                                        panelX, area->y + area->height };
        MoveWindow(g_hGitPanel, panelX, area->y, g_gitPanelWidth, area->height, TRUE);
        area->width -= g_gitPanelWidth + SPLITTER_WIDTH;
    } else {
        g_gitPanelSplitterRect = (RECT){ area->x + g_gitPanelWidth, area->y,
                                        area->x + g_gitPanelWidth + SPLITTER_WIDTH, area->y + area->height };
        MoveWindow(g_hGitPanel, area->x, area->y, g_gitPanelWidth, area->height, TRUE);
        area->x += g_gitPanelWidth + SPLITTER_WIDTH;
        area->width -= g_gitPanelWidth + SPLITTER_WIDTH;
    }
    if (area->width < 0) area->width = 0;
    ShowWindow(g_hGitPanel, SW_SHOW);
    if (g_hGitTree) ShowWindow(g_hGitTree, SW_SHOW);
}

static int git_panel_splitter_contains_x(int x) {
    return g_gitPanelVisible && !g_focusMode &&
           x >= g_gitPanelSplitterRect.left && x <= g_gitPanelSplitterRect.right;
}

static void hide_git_panel(void) {
    if (!g_gitPanelVisible) return;
    g_gitPanelVisible = 0;
    relayout_and_save(g_hMainWnd);
}

/* View > Git Panel (Ctrl+Shift+G), and the status bar's "Open Git Panel". */
static void show_git_panel(void) {
    refresh_git_status();
    render_git_panel();
    if (!g_gitPanelVisible) {
        g_gitPanelVisible = 1;
        relayout_and_save(g_hMainWnd);
    }
    if (g_hGitTree) SetFocus(g_hGitTree);
}

static void toggle_git_panel(void) {
    if (g_gitPanelVisible) hide_git_panel();
    else show_git_panel();
}

static void toggle_git_panel_side(HWND hwnd) {
    g_gitPanelOnRight = !g_gitPanelOnRight;
    relayout_and_save(hwnd);
}
