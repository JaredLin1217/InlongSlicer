param(
    [ValidateSet("Verify", "Cleanup")]
    [string] $Action = "Verify",
    [string[]] $RuntimeIds = @(),
    [string] $ParentThreadId = "",
    [string] $RepoCwd = "",
    [string] $CodexHome = "",
    [int] $DelaySeconds = 5,
    [switch] $Force,
    [switch] $Quiet
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path

if ([string]::IsNullOrWhiteSpace($RepoCwd)) {
    $RepoCwd = $RepoRoot
}
if ([string]::IsNullOrWhiteSpace($CodexHome)) {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $CodexHome = Join-Path $RepoRoot ".codex"
    }
    else {
        $CodexHome = Join-Path $env:USERPROFILE ".codex"
    }
}

function Normalize-PathText {
    param([string] $PathText)
    if ([string]::IsNullOrWhiteSpace($PathText)) {
        return ""
    }
    return ([System.IO.Path]::GetFullPath($PathText).TrimEnd("\", "/")).ToLowerInvariant()
}

function Test-ExactRuntimeId {
    param([string] $RuntimeId)
    if ([string]::IsNullOrWhiteSpace($RuntimeId)) {
        return $false
    }
    if ($RuntimeId -match '[\\/]' -or $RuntimeId -match '\s') {
        return $false
    }
    if ($RuntimeId.Length -lt 12 -or $RuntimeId.Length -gt 160) {
        return $false
    }
    if ($RuntimeId -notmatch '\d') {
        return $false
    }
    return ($RuntimeId -match '^[A-Za-z0-9_.:-]+$')
}

function Get-IdHitCount {
    param(
        [string] $Text,
        [string[]] $Ids
    )
    if ([string]::IsNullOrEmpty($Text)) {
        return 0
    }
    $count = 0
    foreach ($id in $Ids) {
        if ($Text.Contains($id)) {
            $count++
        }
    }
    return $count
}

function Get-PythonCommand {
    $commands = @("python", "py")
    foreach ($candidate in $commands) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($null -ne $cmd) {
            return $cmd.Source
        }
    }
    return ""
}

function Invoke-SqliteCleanup {
    param(
        [string[]] $Ids,
        [string] $ActionName,
        [string] $RepoPath,
        [string] $ParentId,
        [string] $CodexRoot
    )
    $dbFiles = @()
    if (Test-Path -LiteralPath $CodexRoot -PathType Container) {
        $dbFiles = @(Get-ChildItem -LiteralPath $CodexRoot -Filter "state_*.sqlite" -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
    }
    $python = Get-PythonCommand
    if ($dbFiles.Count -eq 0 -or $Ids.Count -eq 0) {
        return [pscustomobject]@{
            available = [bool] (-not [string]::IsNullOrWhiteSpace($python))
            databases = @()
            thread_rows_remaining = 0
            edge_rows_remaining = 0
            rows_deleted = 0
            blocked_ids = @()
        }
    }
    if ([string]::IsNullOrWhiteSpace($python)) {
        if ($ActionName -eq "Cleanup") {
            throw "Python is required for SQLite cleanup because PowerShell has no built-in SQLite provider."
        }
        return [pscustomobject]@{
            available = $false
            databases = @()
            thread_rows_remaining = 0
            edge_rows_remaining = 0
            rows_deleted = 0
            blocked_ids = @()
        }
    }

    $payload = @{
        ids = $Ids
        action = $ActionName
        repo_cwd = $RepoPath
        parent_thread_id = $ParentId
        dbs = $dbFiles
    } | ConvertTo-Json -Compress -Depth 6

    $env:AGENTS_CLEANUP_ARGS = $payload
    $pythonCode = @'
import json
import os
import sqlite3

def norm_path(value):
    if not value:
        return ""
    text = os.path.normcase(os.path.abspath(str(value).rstrip(chr(92) + "/")))
    win_long_prefix = chr(92) + chr(92) + "?" + chr(92)
    if text.startswith(win_long_prefix):
        text = text[len(win_long_prefix):]
    return text

def table_names(conn):
    return {row[0] for row in conn.execute("select name from sqlite_master where type='table'")}

def columns(conn, table):
    return [row[1] for row in conn.execute("pragma table_info(%s)" % table)]

def pick(cols, names):
    lowered = {c.lower(): c for c in cols}
    for name in names:
        if name in lowered:
            return lowered[name]
    return None

def rows_by_id(conn, table, id_col, ids):
    if not id_col:
        return {}
    result = {}
    for rid in ids:
        try:
            rows = conn.execute("select * from %s where %s = ?" % (table, id_col), (rid,)).fetchall()
        except sqlite3.Error:
            rows = []
        result[rid] = rows
    return result

payload = json.loads(os.environ["AGENTS_CLEANUP_ARGS"])
ids = list(dict.fromkeys(payload.get("ids", [])))
action = payload.get("action", "Verify")
repo_cwd = norm_path(payload.get("repo_cwd", ""))
parent_thread_id = payload.get("parent_thread_id") or ""
results = []

for db in payload.get("dbs", []):
    item = {
        "db": db,
        "threads_before": 0,
        "edges_before": 0,
        "dynamic_tools_before": 0,
        "threads_deleted": 0,
        "edges_deleted": 0,
        "dynamic_tools_deleted": 0,
        "threads_after": 0,
        "edges_after": 0,
        "dynamic_tools_after": 0,
        "eligible_ids": [],
        "blocked_ids": [],
        "parent_preserved": True,
        "error": ""
    }
    try:
        conn = sqlite3.connect(db)
        conn.row_factory = sqlite3.Row
        tables = table_names(conn)
        thread_table = "threads" if "threads" in tables else None
        edge_table = "thread_spawn_edges" if "thread_spawn_edges" in tables else None
        dynamic_table = "thread_dynamic_tools" if "thread_dynamic_tools" in tables else None

        thread_id_col = source_col = cwd_col = None
        if thread_table:
            tcols = columns(conn, thread_table)
            thread_id_col = pick(tcols, ["id", "thread_id"])
            source_col = pick(tcols, ["source", "source_kind", "kind", "origin"])
            cwd_col = pick(tcols, ["cwd", "repo_cwd", "working_directory", "worktree", "worktree_path"])
        edge_child_col = edge_parent_col = edge_status_col = None
        if edge_table:
            ecols = columns(conn, edge_table)
            edge_child_col = pick(ecols, ["child_thread_id", "child_id", "thread_id"])
            edge_parent_col = pick(ecols, ["parent_thread_id", "parent_id"])
            edge_status_col = pick(ecols, ["status", "state"])
        dynamic_thread_col = None
        if dynamic_table:
            dcols = columns(conn, dynamic_table)
            dynamic_thread_col = pick(dcols, ["thread_id", "id"])

        thread_rows = rows_by_id(conn, thread_table, thread_id_col, ids) if thread_table else {}
        edge_rows = rows_by_id(conn, edge_table, edge_child_col, ids) if edge_table else {}
        dynamic_rows = rows_by_id(conn, dynamic_table, dynamic_thread_col, ids) if dynamic_table else {}

        item["threads_before"] = sum(len(thread_rows.get(rid, [])) for rid in ids)
        item["edges_before"] = sum(len(edge_rows.get(rid, [])) for rid in ids)
        item["dynamic_tools_before"] = sum(len(dynamic_rows.get(rid, [])) for rid in ids)

        eligible = set()
        blocked = set()
        for rid in ids:
            if parent_thread_id and rid == parent_thread_id:
                blocked.add(rid)
                continue
            rows = thread_rows.get(rid, [])
            edges = edge_rows.get(rid, [])
            source_ok = False
            cwd_ok = False
            if rows and source_col and cwd_col:
                for row in rows:
                    source_text = str(row[source_col] or "").lower()
                    cwd_text = norm_path(row[cwd_col])
                    if "subagent" in source_text or "agent" in source_text:
                        source_ok = True
                    if cwd_text == repo_cwd:
                        cwd_ok = True
            parent_ok = True
            status_ok = True
            if edges and edge_parent_col and parent_thread_id:
                parent_ok = any(str(row[edge_parent_col] or "") == parent_thread_id for row in edges)
            if edges and edge_status_col:
                active = {"active", "running", "created", "queued", "reporting"}
                status_ok = all(str(row[edge_status_col] or "").lower() not in active for row in edges)
            if (rows and source_ok and cwd_ok and parent_ok and status_ok) or (not rows and edges and parent_ok and status_ok):
                eligible.add(rid)
            elif rows or edges or dynamic_rows.get(rid, []):
                blocked.add(rid)

        if action == "Cleanup":
            for rid in eligible:
                if edge_table and edge_child_col:
                    cur = conn.execute("delete from %s where %s = ?" % (edge_table, edge_child_col), (rid,))
                    item["edges_deleted"] += cur.rowcount if cur.rowcount is not None else 0
                if dynamic_table and dynamic_thread_col:
                    cur = conn.execute("delete from %s where %s = ?" % (dynamic_table, dynamic_thread_col), (rid,))
                    item["dynamic_tools_deleted"] += cur.rowcount if cur.rowcount is not None else 0
                if thread_table and thread_id_col:
                    cur = conn.execute("delete from %s where %s = ?" % (thread_table, thread_id_col), (rid,))
                    item["threads_deleted"] += cur.rowcount if cur.rowcount is not None else 0
            conn.commit()

        item["threads_after"] = sum(len(rows_by_id(conn, thread_table, thread_id_col, ids).get(rid, [])) for rid in ids) if thread_table else 0
        item["edges_after"] = sum(len(rows_by_id(conn, edge_table, edge_child_col, ids).get(rid, [])) for rid in ids) if edge_table else 0
        item["dynamic_tools_after"] = sum(len(rows_by_id(conn, dynamic_table, dynamic_thread_col, ids).get(rid, [])) for rid in ids) if dynamic_table else 0
        item["eligible_ids"] = sorted(eligible)
        item["blocked_ids"] = sorted(blocked)
        results.append(item)
    except Exception as exc:
        item["error"] = str(exc)
        results.append(item)
    finally:
        try:
            conn.close()
        except Exception:
            pass

print(json.dumps({"databases": results}, separators=(",", ":")))
'@
    try {
        $output = $pythonCode | & $python "-"
        if ($LASTEXITCODE -ne 0) {
            throw "SQLite helper failed with exit code $LASTEXITCODE."
        }
        $parsed = ($output -join "`n") | ConvertFrom-Json
    }
    finally {
        Remove-Item Env:\AGENTS_CLEANUP_ARGS -ErrorAction SilentlyContinue
    }

    $threadRows = 0
    $edgeRows = 0
    $deleted = 0
    $blocked = @()
    foreach ($db in @($parsed.databases)) {
        $threadRows += [int] $db.threads_after
        $edgeRows += [int] $db.edges_after
        $deleted += [int] $db.threads_deleted + [int] $db.edges_deleted + [int] $db.dynamic_tools_deleted
        foreach ($id in @($db.blocked_ids)) {
            if ($blocked -notcontains $id) {
                $blocked += $id
            }
        }
    }
    return [pscustomobject]@{
        available = $true
        databases = @($parsed.databases)
        thread_rows_remaining = $threadRows
        edge_rows_remaining = $edgeRows
        rows_deleted = $deleted
        blocked_ids = $blocked
    }
}

function Invoke-SessionIndexCleanup {
    param(
        [string[]] $Ids,
        [string] $ActionName,
        [string] $CodexRoot
    )
    $path = Join-Path $CodexRoot "session_index.jsonl"
    $result = [ordered]@{
        path = $path
        before = 0
        deleted = 0
        after = 0
        exists = (Test-Path -LiteralPath $path -PathType Leaf)
    }
    if (-not $result.exists -or $Ids.Count -eq 0) {
        return [pscustomobject]$result
    }
    $lines = @(Get-Content -LiteralPath $path)
    $matched = @()
    $kept = @()
    foreach ($line in $lines) {
        if ((Get-IdHitCount -Text $line -Ids $Ids) -gt 0) {
            $matched += $line
        }
        else {
            $kept += $line
        }
    }
    $result.before = $matched.Count
    if ($ActionName -eq "Cleanup" -and $matched.Count -gt 0) {
        [System.IO.File]::WriteAllLines($path, $kept, [System.Text.UTF8Encoding]::new($false))
        $result.deleted = $matched.Count
    }
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $afterText = Get-Content -LiteralPath $path -Raw
        $result.after = Get-IdHitCount -Text $afterText -Ids $Ids
    }
    return [pscustomobject]$result
}

function Remove-IdsFromJsonValue {
    param(
        $Value,
        [string[]] $Ids
    )
    if ($null -eq $Value) {
        return $null
    }
    if ($Value -is [string]) {
        if ($Ids -contains $Value) {
            return $null
        }
        return $Value
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string] -and $Value -isnot [pscustomobject]) {
        $items = @()
        foreach ($item in $Value) {
            $clean = Remove-IdsFromJsonValue -Value $item -Ids $Ids
            if ($null -ne $clean) {
                $items += $clean
            }
        }
        return ,$items
    }
    if ($Value -is [pscustomobject]) {
        $object = [ordered]@{}
        foreach ($property in $Value.PSObject.Properties) {
            if ($Ids -contains $property.Name) {
                continue
            }
            $clean = Remove-IdsFromJsonValue -Value $property.Value -Ids $Ids
            if ($null -ne $clean) {
                $object[$property.Name] = $clean
            }
        }
        return [pscustomobject]$object
    }
    return $Value
}

function Remove-ExactJsonStringFallback {
    param(
        [string] $Text,
        [string[]] $Ids
    )
    $newText = $Text
    foreach ($id in $Ids) {
        $quotedPattern = '"' + [regex]::Escape($id) + '"'
        $newText = [regex]::Replace($newText, $quotedPattern + '\s*,\s*', "")
        $newText = [regex]::Replace($newText, ',\s*' + $quotedPattern, "")
        $newText = [regex]::Replace($newText, $quotedPattern, "")
    }
    return $newText
}

function Invoke-GlobalStateCleanup {
    param(
        [string[]] $Ids,
        [string] $ActionName,
        [string] $CodexRoot
    )
    $results = @()
    if ($Ids.Count -eq 0 -or -not (Test-Path -LiteralPath $CodexRoot -PathType Container)) {
        return $results
    }
    $files = @(Get-ChildItem -LiteralPath $CodexRoot -Filter ".codex-global-state*.json" -File -ErrorAction SilentlyContinue)
    foreach ($file in $files) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        $before = Get-IdHitCount -Text $text -Ids $Ids
        $deleted = 0
        $fallbackUsed = $false
        $parseError = ""
        if ($ActionName -eq "Cleanup" -and $before -gt 0) {
            try {
                $json = $text | ConvertFrom-Json
                $clean = Remove-IdsFromJsonValue -Value $json -Ids $Ids
                $newText = $clean | ConvertTo-Json -Depth 80 -Compress
                [System.IO.File]::WriteAllText($file.FullName, $newText, [System.Text.UTF8Encoding]::new($false))
                $afterText = Get-Content -LiteralPath $file.FullName -Raw
                $deleted = $before - (Get-IdHitCount -Text $afterText -Ids $Ids)
            }
            catch {
                $parseError = $_.Exception.Message
                $fallbackText = Remove-ExactJsonStringFallback -Text $text -Ids $Ids
                $fallbackAfter = Get-IdHitCount -Text $fallbackText -Ids $Ids
                if ($fallbackAfter -lt $before) {
                    [System.IO.File]::WriteAllText($file.FullName, $fallbackText, [System.Text.UTF8Encoding]::new($false))
                    $deleted = $before - $fallbackAfter
                    $fallbackUsed = $true
                }
            }
        }
        $afterText = Get-Content -LiteralPath $file.FullName -Raw
        $results += [pscustomobject]@{
            path = $file.FullName
            before = $before
            deleted = $deleted
            after = (Get-IdHitCount -Text $afterText -Ids $Ids)
            fallback_used = $fallbackUsed
            parse_error = $parseError
        }
    }
    return $results
}

function Invoke-RolloutCleanup {
    param(
        [string[]] $Ids,
        [string] $ActionName,
        [string] $RepoPath,
        [string] $CodexRoot
    )
    if ($Ids.Count -eq 0) {
        return [pscustomobject]@{
            before = 0
            deleted = 0
            blocked = 0
            after = 0
            ignored_parent_refs = 0
        }
    }
    $roots = @(
        (Join-Path $CodexRoot "sessions"),
        (Join-Path $CodexRoot "archived_sessions")
    )
    $repoLower = $RepoPath.ToLowerInvariant()
    $repoEscaped = ($RepoPath -replace '\\', '\\').ToLowerInvariant()
    $before = 0
    $deleted = 0
    $blocked = 0
    $ignoredParentRefs = 0
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        $files = @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue)
        foreach ($file in $files) {
            $text = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue
            $nameHit = Get-IdHitCount -Text $file.FullName -Ids $Ids
            $contentHit = Get-IdHitCount -Text $text -Ids $Ids
            if (($nameHit + $contentHit) -eq 0) {
                continue
            }
            if ($nameHit -eq 0) {
                $ignoredParentRefs++
                continue
            }
            $before++
            $lower = $text.ToLowerInvariant()
            $safe = ($file.Name.StartsWith("rollout-") -and $file.Extension -eq ".jsonl") -or ($lower.Contains("subagent") -and ($lower.Contains($repoLower) -or $lower.Contains($repoEscaped)))
            if ($ActionName -eq "Cleanup") {
                if ($safe) {
                    Remove-Item -LiteralPath $file.FullName -Force
                    $deleted++
                }
                else {
                    $blocked++
                }
            }
        }
    }
    $after = 0
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue)) {
            if ((Get-IdHitCount -Text $file.FullName -Ids $Ids) -gt 0) {
                $after++
            }
        }
    }
    return [pscustomobject]@{
        before = $before
        deleted = $deleted
        blocked = $blocked
        after = $after
        ignored_parent_refs = $ignoredParentRefs
    }
}

function Invoke-AllChecks {
    param([string] $Mode)
    $sqlite = Invoke-SqliteCleanup -Ids $RuntimeIds -ActionName $Mode -RepoPath $NormalizedRepoCwd -ParentId $ParentThreadId -CodexRoot $NormalizedCodexHome
    $sessionIndex = Invoke-SessionIndexCleanup -Ids $RuntimeIds -ActionName $Mode -CodexRoot $NormalizedCodexHome
    $globalState = @(Invoke-GlobalStateCleanup -Ids $RuntimeIds -ActionName $Mode -CodexRoot $NormalizedCodexHome)
    $rollouts = Invoke-RolloutCleanup -Ids $RuntimeIds -ActionName $Mode -RepoPath $NormalizedRepoCwd -CodexRoot $NormalizedCodexHome
    $globalRemaining = 0
    foreach ($state in $globalState) {
        $globalRemaining += [int] $state.after
    }
    return [pscustomobject]@{
        sqlite_thread_state_result = $sqlite
        session_index_result = $sessionIndex
        global_unread_state_result = $globalState
        rollout_residue_result = $rollouts
        residue_total = ([int]$sqlite.thread_rows_remaining + [int]$sqlite.edge_rows_remaining + [int]$sessionIndex.after + [int]$globalRemaining + [int]$rollouts.after)
    }
}

$RuntimeIds = @($RuntimeIds | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
foreach ($id in $RuntimeIds) {
    if (-not (Test-ExactRuntimeId -RuntimeId $id)) {
        throw "RuntimeIds must be exact runtime ids, never sidebar nicknames: $id"
    }
}
if ($Action -eq "Cleanup") {
    if (-not $Force) {
        throw "Cleanup is destructive and no-backup; pass -Force only after official close proof and exact cleanup authorization."
    }
    if ($RuntimeIds.Count -eq 0) {
        throw "Cleanup requires at least one exact runtime id."
    }
}

$NormalizedRepoCwd = Normalize-PathText -PathText $RepoCwd
$NormalizedCodexHome = Normalize-PathText -PathText $CodexHome
$result = Invoke-AllChecks -Mode $Action
$delayed = $null
if ($Action -eq "Cleanup" -and $DelaySeconds -gt 0) {
    Start-Sleep -Seconds $DelaySeconds
    $delayed = Invoke-AllChecks -Mode "Verify"
}
$globalDeleted = 0
$globalRemaining = 0
foreach ($state in @($result.global_unread_state_result)) {
    $globalDeleted += [int] $state.deleted
    $globalRemaining += [int] $state.after
}

$summary = [pscustomobject]@{
    action = $Action
    no_backup_authorization = [bool] ($Action -eq "Cleanup" -and $Force)
    repo_cwd = $NormalizedRepoCwd
    parent_thread_id = $ParentThreadId
    target_ids = $RuntimeIds
    target_filter = "parent/cwd closed child subagent runtime ids, never sidebar nicknames"
    deleted_counts = @{
        sqlite_rows = [int] $result.sqlite_thread_state_result.rows_deleted
        session_index_rows = [int] $result.session_index_result.deleted
        global_unread_entries = $globalDeleted
        rollout_files = [int] $result.rollout_residue_result.deleted
    }
    target_counts = @{
        sqlite_thread_or_edge_rows = ([int] $result.sqlite_thread_state_result.thread_rows_remaining + [int] $result.sqlite_thread_state_result.edge_rows_remaining)
        session_index_rows = [int] $result.session_index_result.after
        global_unread_entries = $globalRemaining
        rollout_files = [int] $result.rollout_residue_result.after
    }
    sqlite_thread_state_result = $result.sqlite_thread_state_result
    session_index_result = $result.session_index_result
    global_unread_state_result = $result.global_unread_state_result
    rollout_residue_result = $result.rollout_residue_result
    delayed_zero_residue = if ($null -eq $delayed) { $null } else { [bool] ($delayed.residue_total -eq 0) }
    delayed_verification = $delayed
    parent_preserved = $true
    desktop_reload_or_restart_result = "not performed by helper"
}

if (-not $Quiet) {
    $summary | ConvertTo-Json -Depth 80
}
