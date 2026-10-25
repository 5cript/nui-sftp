#!/usr/bin/env python3
"""Turns the profile a first start of nui-sftp wrote into the screenshot profile.

Usage: seed_profile.py <persistence.devel.json> <command_store.devel.db> <demo home> <remote host> <remote port>
                       <remote user> <remote root> <key file name>

Replaces the sessions and layouts, skips onboarding, unpauses the queue and backdates a few command history rows.
The app has to be stopped while this runs, it rewrites both files on start.
"""

import json
import os
import sqlite3
import sys
import time

LAYOUT_NAME = "Workspace"


def tabArea(*widgets):
    return {"type": "tab-area", "currentIndex": 0, "widgets": list(widgets)}


def splitArea(orientation, sizes, *children):
    return {"type": "split-area", "orientation": orientation, "sizes": sizes, "children": list(children)}


def sshLayout():
    return {
        "__extra": {"fileGrid": {"leftSide": {"flavor": "icons"}, "rightSide": {"flavor": "icons"}}},
        "main": splitArea(
            "horizontal",
            [0.4, 0.6],
            splitArea("vertical", [0.6, 0.4], tabArea("terminal"), tabArea("operation-queue", "file-tracking")),
            tabArea("file-explorer", "command-history", "command-snippets"),
        ),
    }


def shellLayout():
    return {
        "__extra": {"fileGrid": {"leftSide": {"flavor": "icons"}, "rightSide": {"flavor": "icons"}}},
        "main": splitArea(
            "horizontal",
            [0.5, 0.5],
            tabArea("terminal", "command-history", "command-snippets"),
            tabArea("file-explorer"),
        ),
    }


def referenced(**overrides):
    return {"$ref": "default", **overrides}


def termios():
    return referenced(controlFlags={}, inputFlags={}, localFlags={}, outputFlags={})


def sshSession(order, host, port, user, privateKey, sshDirectory, remoteFavorites=(), defaultDirectory=None):
    sftpOptions = referenced(defaultDirectory=defaultDirectory) if defaultDirectory else referenced()
    return {
        "engine": {
            "host": host,
            "maxReconnectAttempts": -1,
            "maxReconnectBackoffMs": 16000,
            "openSftpByDefault": True,
            "port": port,
            "remoteFavorites": list(remoteFavorites),
            "sftpOptions": sftpOptions,
            "sshKeyPrivate": privateKey,
            "sshKeyPublic": privateKey + ".pub",
            "sshOptions": referenced(
                bypassConfig=True,
                environment={},
                identities=[],
                sshDirectory=sshDirectory,
                tryAgentForAuthentication=False,
                usePasswordAuth=False,
                usePublicKeyAutoAuth=False,
            ),
            "user": user,
            "users": [{"name": user, "sshKeyPrivate": privateKey, "sshKeyPublic": privateKey + ".pub"}],
        },
        "historyOptions": referenced(),
        "icon": "favorite",
        "layouts": {},
        "orderBy": order,
        "queueOptions": referenced(autoRemoveCompletedOperations=False, liveQueuePageSize=200, startInPausedState=False),
        "startupSession": False,
        "terminalOptions": referenced(),
        "termios": termios(),
        "type": "ssh",
    }


def shellSession(order, home):
    prompt = r"\[\e[1;32m\]demo@workstation\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ "
    return {
        "engine": {
            "arguments": ["--noprofile", "--norc", "-i"],
            "cleanEnvironment": False,
            "command": "/bin/bash",
            "environment": {
                "HOME": home,
                "HISTFILE": f"{home}/.bash_history",
                "PS1": prompt,
                "PAGER": "cat",
                "GIT_PAGER": "cat",
                "TERM": "xterm-256color",
            },
            "exitTimeoutSeconds": 5,
            "isPty": True,
        },
        "historyOptions": referenced(),
        "icon": "home",
        "layouts": {},
        "orderBy": order,
        "queueOptions": referenced(),
        "startupSession": False,
        "terminalOptions": referenced(),
        "termios": termios(),
        "type": "shell",
    }


def patchState(path, home, host, port, user, remoteRoot, keyName):
    with open(path, encoding="utf-8") as file:
        state = json.load(file)

    sshDirectory = f"{home}/.ssh"
    state["sessions"] = {
        "demo-server": sshSession(
            "A",
            host,
            port,
            user,
            f"{sshDirectory}/{keyName}",
            sshDirectory,
            remoteFavorites=[f"{remoteRoot}/website", f"{remoteRoot}/backups"],
            defaultDirectory=f"{remoteRoot}/website",
        ),
        "Local Shell": shellSession("B", home),
        "staging": sshSession("C", "staging.example.com", 22, "deploy", f"{sshDirectory}/id_ed25519", sshDirectory),
        "backup": sshSession("D", "backup.example.net", 2222, "backup", f"{sshDirectory}/id_ed25519", sshDirectory),
    }
    state["sessionLayouts"] = {"ssh": {LAYOUT_NAME: sshLayout()}, "shell": {LAYOUT_NAME: shellLayout()}}

    interface = state.setdefault("uiOptions", {})
    interface["onboardingCompleted"] = True
    interface["darkLightMode"] = "Dark"
    interface["localFavorites"] = [f"{home}/Projects/website", f"{home}/Documents"]
    interface["showHiddenFilesLocally"] = False
    interface["showHiddenFilesRemotely"] = False
    neverShowAgain = interface.setdefault("neverShowAgainDialogs", [])
    if "persistenceLoadWarning" not in neverShowAgain:
        neverShowAgain.append("persistenceLoadWarning")

    state.setdefault("localizationOptions", {})["languageCode"] = "en_US"

    queue = state.setdefault("queueOptions", {}).setdefault("default", {})
    queue["startInPausedState"] = False
    queue["autoRemoveCompletedOperations"] = False

    ssh = state.setdefault("sshOptions", {}).setdefault("default", {})
    ssh["sshDirectory"] = sshDirectory
    ssh["identities"] = []
    ssh["bypassConfig"] = True

    with open(path, "w", encoding="utf-8") as file:
        json.dump(state, file, indent=4)
        file.write("\n")


def seedHistory(path, host, user):
    hour = 3600
    day = 24 * hour
    if not os.path.exists(path):
        print(f"seed_profile: the first start wrote no {path}, history stays empty")
        return
    demoHost = f"{user}@{host}"
    rows = [
        (demoHost, "sudo systemctl restart nginx", 6 * day, 12, True, False),
        (demoHost, "docker compose ps", 2 * hour, 25, False, True),
        (demoHost, "docker compose logs -f --tail 100 web", 3 * hour, 9, False, False),
        (demoHost, "df -h /", 1 * day, 7, False, False),
        (demoHost, "du -sh ~/demo/*", 1 * day + 2 * hour, 4, False, False),
        (demoHost, "grep -c ' 404 ' ~/demo/logs/access.log", 2 * day, 3, False, False),
        (demoHost, "journalctl -u nginx --since today", 3 * day, 6, False, False),
        (demoHost, "tar -czf website-backup.tar.gz website", 5 * day, 2, False, True),
        ("deploy@staging.example.com", "git pull --ff-only", 4 * hour, 31, False, False),
        ("deploy@staging.example.com", "npm run build", 4 * hour, 28, False, False),
        ("deploy@staging.example.com", "certbot renew --dry-run", 9 * day, 2, True, False),
        ("deploy@staging.example.com", "systemctl status nginx", 2 * day, 11, False, False),
        ("/bin/bash", "go test ./...", 5 * hour, 19, False, False),
        ("/bin/bash", "make build", 5 * hour, 14, False, False),
        ("/bin/bash", "ssh-keygen -t ed25519 -C demo@example.com", 12 * day, 1, False, False),
    ]
    connection = sqlite3.connect(path)
    try:
        columns = {row[1] for row in connection.execute("PRAGMA table_info(history)")}
        required = {"host", "command", "first_run", "last_run", "runs", "pinned", "favorite"}
        if not required.issubset(columns):
            print(f"seed_profile: {path} has no history table with {sorted(required)}, history stays empty")
            return
        now = int(time.time())
        connection.executemany(
            "INSERT OR IGNORE INTO history (host, command, first_run, last_run, runs, pinned, favorite) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            [
                (rowHost, command, now - age - 20 * day, now - age, runs, int(pinned), int(favorite))
                for rowHost, command, age, runs, pinned, favorite in rows
            ],
        )
        connection.commit()
    finally:
        connection.close()


def main():
    if len(sys.argv) != 9:
        sys.exit(__doc__)
    statePath, databasePath, home, host, port, user, remoteRoot, keyName = sys.argv[1:]
    patchState(statePath, home, host, int(port), user, remoteRoot, keyName)
    seedHistory(databasePath, host, user)


if __name__ == "__main__":
    main()
