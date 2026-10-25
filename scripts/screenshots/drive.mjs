// Drives a running CEF build of nui-sftp through the screenshot scenes and writes one raw PNG per scene.
// Started by take.sh, which prepares the demo profile, the demo files and the app. Run it alone against an
// instance take.sh left running (--keep-running) to iterate on a scene.
//
// Usage: node drive.mjs --port <debug port> --output <raw png directory> [--scenes a,b] [--width 1900]
//        [--height 1000] [--scale 1] [--home /home/demo] [--remote-root /home/sftpuser/demo]
//        [--remote-user sftpuser] [--remote-host 192.168.178.70] [--remote-port 5000]
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { DevTools } from './cdp.mjs';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));

const { values: options } = parseArgs({
    options: {
        port: { type: 'string' },
        output: { type: 'string' },
        scenes: { type: 'string', default: '' },
        width: { type: 'string', default: '1900' },
        height: { type: 'string', default: '1000' },
        scale: { type: 'string', default: '1' },
        home: { type: 'string', default: '/home/demo' },
        'remote-root': { type: 'string', default: '/home/sftpuser/demo' },
        'remote-user': { type: 'string', default: 'sftpuser' },
        'remote-host': { type: 'string', default: '192.168.178.70' },
        'remote-port': { type: 'string', default: '5000' },
    },
});
if (!options.port || !options.output) {
    console.error('usage: node drive.mjs --port <debug port> --output <directory> [options], see the header');
    process.exit(2);
}

const serverName = 'demo-server';
const shellName = 'Local Shell';
const layoutName = 'Workspace';
const home = options.home;
const remoteRoot = options['remote-root'];
const remoteHome = `/home/${options['remote-user']}`;
const remoteRootFromHome = remoteRoot.startsWith(`${remoteHome}/`) ? `~${remoteRoot.slice(remoteHome.length)}` : remoteRoot;
const sshTabTitle = `${options['remote-user']}@${options['remote-host']}:${options['remote-port']}`;
const presetFolders = ['Docker', 'Git', 'systemd', 'Kubernetes', 'Networking', 'Logs', 'Nginx', 'SSH', 'Processes', 'System'];

mkdirSync(options.output, { recursive: true });
const pageDriverSource = readFileSync(join(scriptDirectory, 'page_driver.js'), 'utf8');
const devTools = await DevTools.connect(options.port);

const page = async (functionName, ...parameters) => {
    const loaded = await devTools.evaluate('typeof globalThis.screenshotDriver === "object"');
    if (!loaded)
        await devTools.evaluate(pageDriverSource);
    return devTools.call('screenshotDriver', functionName, ...parameters);
};

const log = (message) => console.log(`[drive] ${message}`);

const capture = async (name) => {
    await page('prepareCapture');
    const path = join(options.output, `${name}.png`);
    await devTools.screenshot(path);
    log(`captured ${path}`);
};

let sshTerminalPrepared = false;

/**
 * @brief Opens (or selects) the SSH session and types a few commands once, so terminal and history have content.
 */
const ensureSshSession = async () => {
    await page('openSession', serverName, layoutName, sshTabTitle, true);
    if (sshTerminalPrepared)
        return;
    await page('clearTerminal', 'terminal');
    await page('typeCommand', 'terminal', `cd ${remoteRootFromHome}/website`);
    await page('typeCommand', 'terminal', 'ls -la');
    await page('typeCommand', 'terminal', 'tail -n 5 ../logs/access.log | cut -c1-84');
    sshTerminalPrepared = true;
};

const showProject = async () => {
    await page('activatePanel', 'file-explorer');
    await page('activatePanel', 'terminal');
    await page('navigate', 0, `${home}/Projects/website`, 'index.html');
    await page('navigate', 1, `${remoteRoot}/website`, 'index.html');
};

let releasesDownloaded = false;

const downloadReleases = async () => {
    if (releasesDownloaded)
        return;
    await page('activatePanel', 'file-explorer');
    await page('activatePanel', 'operation-queue');
    await page('navigate', 0, `${home}/Downloads`);
    await page('navigate', 1, `${remoteRoot}/releases`, 'website-1.4.1.tar.gz');
    await page('select', 1, 'website-1.4.0.tar.gz');
    await page('select', 1, 'website-1.4.1.tar.gz', true);
    await page('contextAction', 1, 'website-1.4.1.tar.gz', 'Download', true);
    await page('waitQueueDone');
    releasesDownloaded = true;
};

const scenes = {
    session: async () => {
        await ensureSshSession();
        await downloadReleases();
        await page('activatePanel', 'operation-queue');
        await showProject();
        await capture('session');
    },

    history: async () => {
        await ensureSshSession();
        await page('activatePanel', 'command-history');
        await page('waitPanelText', 'command-history', ['docker compose ps']);
        await capture('history');
    },

    snippets: async () => {
        await ensureSshSession();
        await page('activatePanel', 'command-snippets');
        await page('addPresetFolders', presetFolders);
        await page('selectSnippetFolder', 'Git');
        await capture('snippets');
    },

    synchronize: async () => {
        await ensureSshSession();
        await page('activatePanel', 'file-explorer');
        await page('navigate', 0, `${home}/Projects`, 'website');
        await page('select', 0, 'website');
        await page('navigate', 1, remoteRoot, 'website');
        await page('contextAction', 1, 'website', 'Synchronize...');
        log(`sync result: ${await page('waitSyncResult')}`);
        await capture('synchronize');
        await page('closeSyncDialog');
    },

    download_as_archive: async () => {
        await ensureSshSession();
        await page('activatePanel', 'file-explorer');
        await page('navigate', 0, `${home}/Downloads`);
        await page('navigate', 1, remoteRoot, 'website');
        await page('contextAction', 1, 'website', 'Download as Archive');
        await page('waitArchiveDialog');
        await capture('download_as_archive');
        await page('cancelArchiveDialog');
    },

    live_edit: async () => {
        const drafts = ['release-notes.md', 'roadmap.md'];
        await ensureSshSession();
        await page('activatePanel', 'file-explorer');
        await page('activatePanel', 'file-tracking');
        await page('navigate', 0, `${home}/Documents/drafts`, drafts[0]);
        await page('navigate', 1, `${remoteRoot}/docs`);
        for (const draft of drafts)
            await page('contextAction', 0, draft, 'Upload and Watch', true);
        await page('waitPanelText', 'file-tracking', drafts);
        await page('navigate', 1, `${remoteRoot}/docs`, drafts[drafts.length - 1]);
        await capture('live_edit');
    },

    theming: async () => {
        await ensureSshSession();
        await downloadReleases();
        await page('activatePanel', 'operation-queue');
        await showProject();
        await page('setColorMode', 'light');
        try {
            await capture('theming');
        } finally {
            await page('setColorMode', 'dark');
        }
    },

    settings: async () => {
        await page('openSettings');
        try {
            await page('selectSettingsSection', serverName);
            await page('searchSettings', 'timeout', 1);
            await capture('settings');
        } finally {
            await page('closeSettings');
        }
    },

    local: async () => {
        await page('openSession', shellName, layoutName, shellName, false);
        await page('clearTerminal', 'terminal');
        await page('typeCommand', 'terminal', 'cd ~/Projects/api-server');
        await page('typeCommand', 'terminal', 'git log --oneline --graph --decorate -12');
        await page('typeCommand', 'terminal', 'ls -F --color=auto');
        await page('navigate', 0, `${home}/Projects/api-server`, 'README.md');
        await capture('local');
    },
};

const requested = options.scenes ? options.scenes.split(',').map((each) => each.trim()).filter(Boolean) : Object.keys(scenes);
const unknown = requested.filter((name) => !scenes[name]);
if (unknown.length > 0) {
    console.error(`unknown scenes: ${unknown.join(', ')}; known: ${Object.keys(scenes).join(', ')}`);
    process.exit(2);
}

await devTools.send('Emulation.setDeviceMetricsOverride', {
    width: Number(options.width),
    height: Number(options.height),
    deviceScaleFactor: Number(options.scale),
    mobile: false,
});
log(`viewport ${JSON.stringify(await page('viewport'))}`);
await page('assertNoDialog');

const failures = [];
for (const name of requested) {
    log(`scene ${name}`);
    try {
        await scenes[name]();
    } catch (error) {
        failures.push(name);
        console.error(`[drive] scene ${name} failed: ${error.message}`);
        await devTools.screenshot(join(options.output, `${name}.failed.png`)).catch(() => {});
        const left = await page('resetUi').catch((resetError) => resetError.message);
        if (left)
            console.error(`[drive] still open after the reset: ${left}`);
    }
}
devTools.close();
if (failures.length > 0) {
    console.error(`[drive] failed scenes: ${failures.join(', ')} (see the .failed.png captures)`);
    process.exit(1);
}
