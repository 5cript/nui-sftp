#!/usr/bin/env node


// https://github.com/mscdex/ssh2/blob/master/SFTP.md

import ssh2 from 'ssh2';
import util from 'node:util';
import fs from 'node:fs';
import { nanoid } from 'nanoid';
import minimist from 'minimist';
import { makeLogger } from './source/log.mjs';
import { humanFileSize } from './source/human_file_size.mjs';
import CommandLineInterface from './source/cli.mjs';
import { makeSemiHexString } from './source/make_semi_hex_string.mjs';
import { exit } from 'node:process';
import crypto from 'node:crypto';
import { file, directory, symlink, finalizeFakeFs, renameInFakeFs } from './source/fake_file_system.mjs';
import { makeLongName } from './source/sftp_longname.mjs';
import { log } from 'node:console';
import pathUtil from 'node:path';

const { Server, sftp } = ssh2;
const { OPEN_MODE, STATUS_CODE } = ssh2.utils.sftp;

const args = minimist(process.argv.slice(2), {
    alias: {
        p: 'port',
        v: 'verbose',
        l: 'log-file'
    },
    default: {
        port: 0,
        verbose: false,
        'log-file': undefined
    }
});

const port = args.port;
const logMessage = makeLogger(args.verbose, args['log-file']);
logMessage('Starting SFTP server');

const allowedUser = Buffer.from('test');
const allowedPassword = Buffer.from('test');

function checkValue(input, allowed) {
    const autoReject = (input.length !== allowed.length);
    if (autoReject) {
        // Prevent leaking length information by always making a comparison with the
        // same input when lengths don't match what we expect ...
        allowed = input;
    }
    const isMatch = crypto.timingSafeEqual(input, allowed);
    return (!autoReject && isMatch);
}

const fakeFilesystem = finalizeFakeFs({
    ...directory({ name: '' }, [
        directory({ name: 'home' }, [
            directory({ name: 'test' }, [
                directory({ name: 'Documents' }, [
                    file('doc1.txt', 'Document 1 content'),
                    file('doc2.txt', 'Document 2 content')
                ]),
                directory({ name: 'Downloads' }, [
                    file('file1.zip', 'Fake zip content'),
                    file('file2.tar.gz', 'Fake tar.gz content')
                ]),
                directory({ name: 'Pictures' }, [
                    file('image1.png', 'Fake image content'),
                    file('image2.jpg', 'Fake image content')
                ]),
                directory({ name: 'Music' }, [
                    file('song1.mp3', 'Fake song content'),
                    file('song2.wav', 'Fake song content')
                ]),
                directory({ name: 'Videos' }, [
                    file('video1.mp4', 'Fake video content'),
                    file('video2.avi', 'Fake video content')
                ]),
                directory({ name: 'Desktop' }, [
                    file('shortcut1.lnk', 'Fake shortcut content'),
                    file('shortcut2.lnk', 'Fake shortcut content')
                ]),
                symlink('link_to_doc1.txt', '/home/user/Documents/doc1.txt'),
                // Fixtures used by upload/download symlink tests:
                symlink('link_to_file1.txt', '/home/test/file1.txt'),
                symlink('link_to_documents', '/home/test/Documents'),
                symlink('dangling_link', '/home/test/does_not_exist.txt'),
                file('file1.txt', 'Fake file content'),
                file('file2.txt', 'Fake file content'),
                file('large.txt', (() => {
                    const size = 65536;
                    const repeatable = 'abcdefghijklmnopqrstuvwxyz';
                    const division = Math.floor(size / repeatable.length);
                    const mod = size % repeatable.length;
                    return repeatable.repeat(division) + repeatable.slice(0, mod);
                })())
            ]),
            directory({ name: 'deutsch' }, [
                directory({ name: '.config' }, [
                    file('user-dirs.dirs', [
                        '# This file is written by xdg-user-dirs-update',
                        'XDG_DESKTOP_DIR="$HOME/Schreibtisch"',
                        'XDG_DOWNLOAD_DIR="$HOME/Downloads"',
                        'XDG_TEMPLATES_DIR="$HOME/"',
                        'XDG_DOCUMENTS_DIR="$HOME/Dokumente"',
                        'XDG_MUSIC_DIR="/srv/musik"',
                        'XDG_PICTURES_DIR="$HOME/Bilder"',
                        'XDG_VIDEOS_DIR="$HOME/Videos"',
                        ''
                    ].join('\n'))
                ]),
                directory({ name: 'Schreibtisch' }, []),
                directory({ name: 'Downloads' }, []),
                directory({ name: 'Dokumente' }, []),
                directory({ name: 'Bilder' }, []),
                directory({ name: 'Videos' }, [])
            ])
        ]),
        directory({ name: 'Users' }, [
            directory({ name: 'test' }, [
                file('mac.txt', 'Fake mac content')
            ])
        ]),
        directory({ name: 'etc' }, [
            file('passwd', 'Fake passwd content'),
            file('hosts', 'Fake hosts content')
        ]),
        directory({ name: 'var' }, [
            directory({ name: 'log' }, [
                file('syslog', 'Fake syslog content'),
                file('auth.log', 'Fake auth.log content')
            ])
        ])
    ])
});

logMessage(fakeFilesystem);
logMessage(`Find /home/test/Documents: ${util.inspect(fakeFilesystem.find('/home/test/Documents'))}`);

class Handle {
    constructor(path, openType) {
        this.id = nanoid();
        this.path = path;
        this.remainingReadDirEntries = null;
        this.openType = openType;

        logMessage(`Handle created for ${path} with id ${this.id}`);

        if (this.openType !== 'file' && this.openType !== 'dir') {
            throw new Error('Invalid openType').trace();
        }
    }

    idAsBuffer() {
        return Buffer.from(this.id, 'utf8');
    }
}

let defaultPath = '/home/test';
// 'conforming', 'relative', 'empty' or 'failure'. Emulates servers answering REALPATH "." incorrectly.
let realpathDotMode = 'conforming';

// Fault injection and server behaviour switches, configured over the command line interface.
const faults = {
    failReadsAfter: null,
    failWritesAfter: null,
    dropAfterBytes: null,
    failRenames: false,
    // Renames that still succeed before failRenames applies, null for none.
    renamesBeforeFailure: null,
    deniedDirectories: new Set(),
    deniedStats: new Set(),
    bytesRead: 0,
    bytesWritten: 0,
};
// Extension name to extension data, announced in the VERSION packet.
const advertisedExtensions = {};

// True when this rename has to fail. Counts down the renames allowed before failing.
const renameFails = () => {
    if (!faults.failRenames)
        return false;
    if (faults.renamesBeforeFailure !== null && faults.renamesBeforeFailure > 0) {
        --faults.renamesBeforeFailure;
        return false;
    }
    return true;
};

const lengthPrefixed = (text) => {
    const body = Buffer.from(text, 'utf8');
    const header = Buffer.alloc(4);
    header.writeUInt32BE(body.length);
    return Buffer.concat([header, body]);
};

const readLengthPrefixedStrings = (data, count) => {
    const strings = [];
    let offset = 0;
    for (let index = 0; index < count; ++index) {
        if (!data || offset + 4 > data.length)
            return undefined;
        const length = data.readUInt32BE(offset);
        offset += 4;
        if (offset + length > data.length)
            return undefined;
        strings.push(data.toString('utf8', offset, offset + length));
        offset += length;
    }
    return strings;
};

// ssh2 always answers INIT with a VERSION packet without extensions, so the packet is replaced
// on its way out when extensions are advertised.
const advertiseExtensions = (sftpStream) => {
    const names = Object.keys(advertisedExtensions);
    if (names.length === 0)
        return;
    const protocol = sftpStream._protocol;
    const originalChannelData = protocol.channelData;
    protocol.channelData = function (id, data) {
        const versionPacketType = 2;
        if (id === sftpStream.outgoing.id && data.length === 9 && data[4] === versionPacketType) {
            protocol.channelData = originalChannelData;
            const parts = [Buffer.from([versionPacketType, 0, 0, 0, 3])];
            for (const name of names)
                parts.push(lengthPrefixed(name), lengthPrefixed(advertisedExtensions[name]));
            const body = Buffer.concat(parts);
            const header = Buffer.alloc(4);
            header.writeUInt32BE(body.length);
            return originalChannelData.call(protocol, id, Buffer.concat([header, body]));
        }
        return originalChannelData.call(protocol, id, data);
    };
};

const resolveFileName = (path) => {
    if (path === '.') {
        return defaultPath;
    }
    if (path.startsWith('/')) {
        return path;
    }
    const nonNormalized = pathUtil.join(defaultPath, path);
    const normalized = pathUtil.normalize(nonNormalized);
    const linuxified = normalized.replace(/\\/g, '/');
    return linuxified;
}

const server = new Server({
    hostKeys: [fs.readFileSync('key.private')],
    debug: (message) => {
        logMessage(`Debug: ${message}`);
    }
}, (client) => {
    logMessage('Client connected!');
    // SFTP is strictly request/response with one write in flight per file on the client
    // side. With Nagle enabled every 32 KiB write stalls for a delayed-ACK period, which
    // capped the mock at roughly 300 KB/s and made larger transfers hit the kill timer.
    client.setNoDelay(true);

    client.on('authentication', (ctx) => {
        let allowed = true;
        if (!checkValue(Buffer.from(ctx.username), allowedUser))
            allowed = false;

        logMessage(`User and password: ${ctx.username} ${ctx.password}`);
        logMessage(`Method: ${ctx.method}`);

        switch (ctx.method) {
            case 'password':
                {
                    if (!checkValue(Buffer.from(ctx.password), allowedPassword)) {
                        logMessage('Rejecting password');
                        return ctx.reject();
                    }
                    else {
                        logMessage('Accepting password');
                        ctx.accept();
                    }
                    break;
                }
            default:
                {
                    logMessage('Accepting any other method');
                    return ctx.accept();
                }
        }
    });

    client.on('ready', () => {
        logMessage('Client authenticated!');

        client.on('session', (accept, reject) => {
            const session = accept();

            session.on('sftp', (accept, reject) => {
                logMessage('Client SFTP session');

                const handles = new Map();
                const sftpStream = accept();
                advertiseExtensions(sftpStream);
                sftpStream.on('OPEN', (reqid, filename, flags, attrs) => {
                    filename = resolveFileName(filename);

                    logMessage('OPEN', filename, flags, attrs);

                    let result = fakeFilesystem.find(filename);
                    // OPEN follows symlinks like a real server.
                    if (result !== undefined && result.type === 'symlink') {
                        filename = result.target;
                        result = fakeFilesystem.find(filename);
                    }
                    if (result === undefined) {
                        const anyWriteFlag = flags & (OPEN_MODE.WRITE | OPEN_MODE.TRUNC | OPEN_MODE.CREAT | OPEN_MODE.APPEND);
                        if (anyWriteFlag) {
                            const parentPath = pathUtil.dirname(filename);
                            const parent = fakeFilesystem.find(parentPath);
                            if (parent === undefined || parent.type !== 'directory') {
                                logMessage('Parent not found');
                                return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                            }

                            const name = pathUtil.basename(filename);
                            parent.insert(file(name, '', attrs));
                        }
                        else if (flags & OPEN_MODE.READ) {
                            logMessage('File not found');
                            return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                        }
                    } else {
                        if (result.type !== 'file') {
                            logMessage('Not a file');
                            return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                        }

                        if (flags & OPEN_MODE.WRITE) {
                            if (flags & OPEN_MODE.TRUNC) {
                                result.content = Buffer.alloc(0);
                                result.stat.size = 0;
                            }
                        }
                    }

                    const handle = new Handle(filename, 'file');
                    handles.set(handle.id, handle);
                    sftpStream.handle(reqid, handle.idAsBuffer());
                });

                sftpStream.on('WRITE', (reqid, handleRaw, offset, data) => {
                    const handleString = handleRaw.toString('utf8');
                    if (!handles.has(handleString)) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }
                    const handle = handles.get(handleString);
                    if (!handle) {
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    // Fake the write operation
                    const result = fakeFilesystem.find(handle.path);
                    if (result === undefined) {
                        logMessage('File not found');
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }

                    const size = result.stat.size;
                    if (offset > size) {
                        logMessage('Invalid offset');
                        return sftpStream.status(reqid, STATUS_CODE.EOF);
                    }

                    if (faults.dropAfterBytes !== null && faults.bytesRead + faults.bytesWritten >= faults.dropAfterBytes) {
                        logMessage('Fault: dropping the connection during WRITE');
                        return client.end();
                    }
                    if (faults.failWritesAfter !== null && faults.bytesWritten >= faults.failWritesAfter) {
                        logMessage('Fault: failing WRITE');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }
                    faults.bytesWritten += data.length;

                    // Simulate writing data to the file at the specified offset.
                    // Keep content as Buffer end-to-end; converting to UTF-8 strings
                    // here would corrupt any byte > 0x7F (replacement char), which
                    // breaks every binary payload (gzip, bzip2, zstd, ...).
                    // Overwrite in place when the write fits; only grow the buffer
                    // when the write extends past the current end.
                    const end = offset + data.length;
                    if (end <= result.content.length) {
                        data.copy(result.content, offset);
                    } else {
                        result.content = Buffer.concat([result.content.slice(0, offset), data]);
                    }
                    result.stat.size = result.content.length;

                    sftpStream.status(reqid, STATUS_CODE.OK);

                    logMessage(`Write to file at offset ${offset}: ${data.length} bytes`);
                });

                sftpStream.on('READ', (reqid, handleRaw, offset, length) => {
                    logMessage('READ: ', handleRaw, offset, length);

                    const handleString = handleRaw.toString('utf8');
                    if (!handles.has(handleString)) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const handle = handles.get(handleString);
                    if (!handle) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const result = fakeFilesystem.find(handle.path);
                    if (result === undefined) {
                        logMessage('File not found');
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }

                    const size = result.stat.size;
                    if (offset >= size) {
                        logMessage(`Offset ${offset} beyond end of file ${size}: sending EOF.`);
                        return sftpStream.status(reqid, STATUS_CODE.EOF);
                    }

                    // result.content is a Buffer; slice() returns a Buffer view —
                    // send it directly without UTF-8 round-tripping.
                    if (faults.dropAfterBytes !== null && faults.bytesRead + faults.bytesWritten >= faults.dropAfterBytes) {
                        logMessage('Fault: dropping the connection during READ');
                        return client.end();
                    }
                    if (faults.failReadsAfter !== null && faults.bytesRead >= faults.failReadsAfter) {
                        logMessage('Fault: failing READ');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const data = result.content.slice(offset, Math.min(offset + length, size));
                    faults.bytesRead += data.length;
                    logMessage(`Read ${data.length} bytes from offset ${offset}`);

                    sftpStream.data(reqid, data);
                });

                sftpStream.on('CLOSE', (reqid, handleRaw) => {
                    const handleString = handleRaw.toString('utf8');
                    if (!handles.has(handleString)) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }
                    handles.delete(handleString);

                    sftpStream.status(reqid, STATUS_CODE.OK);

                    logMessage('File closed');
                });

                sftpStream.on('REALPATH', (reqid, path) => {
                    if (path === '.' && realpathDotMode !== 'conforming') {
                        if (realpathDotMode === 'failure')
                            return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                        const filename = realpathDotMode === 'relative' ? 'test' : '';
                        return sftpStream.name(reqid, [{ filename, longname: filename, attrs: {} }]);
                    }

                    path = resolveFileName(path);

                    const result = fakeFilesystem.find(path);

                    if (result === undefined) {
                        logMessage(`Path not found: ${path}`);
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }

                    logMessage('REALPATH', result.path(), result);

                    const name = [{
                        filename: result.path(),
                        longname: makeLongName(result),
                        attrs: result.stat
                    }];

                    logMessage(name);

                    sftpStream.name(reqid, name);
                });

                sftpStream.on('STAT', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('STAT', path);

                    const result = fakeFilesystem.find(path);
                    if (result === undefined) {
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }

                    // STAT follows symlinks — return the target's attrs (or NO_SUCH_FILE for
                    // a dangling link). Only ONE reply must be sent per request.
                    if (result.type === 'symlink') {
                        const find2 = fakeFilesystem.find(result.target);
                        if (find2 === undefined) {
                            logMessage('Target not found (dangling link)');
                            return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                        }
                        return sftpStream.attrs(reqid, find2.stat);
                    }

                    sftpStream.attrs(reqid, result.stat);
                });

                sftpStream.on('OPENDIR', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('OPENDIR', path);

                    if (faults.deniedDirectories.has(path)) {
                        logMessage('Fault: permission denied');
                        return sftpStream.status(reqid, STATUS_CODE.PERMISSION_DENIED);
                    }

                    const result = fakeFilesystem.find(path);
                    if (result === undefined || result.type !== 'directory') {
                        logMessage('Not a directory');
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }

                    const handle = new Handle(path, 'dir');
                    handles.set(handle.id, handle);

                    logMessage(`Opening directory with handle ${handle.id}`);
                    sftpStream.handle(reqid, handle.idAsBuffer());
                });

                sftpStream.on('READDIR', (reqid, handleRaw) => {
                    const handleString = handleRaw.toString('utf8');
                    if (!handles.has(handleString)) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }
                    const handle = handles.get(handleString);
                    if (!handle) {
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    let path;
                    if (handle.remainingReadDirEntries === null) {
                        path = handle.path;

                        const result = fakeFilesystem.find(path);

                        if (result === undefined || result.type !== 'directory') {
                            logMessage('READDIR', path, 'Not a directory');
                            return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                        }

                        handle.remainingReadDirEntries = result.children.map((child) => {
                            return {
                                filename: child.name,
                                longname: makeLongName(child),
                                attrs: child.stat
                            };
                        });
                    }

                    if (Array.isArray(handle.remainingReadDirEntries) && handle.remainingReadDirEntries.length !== 0) {
                        const entries = handle.remainingReadDirEntries.splice(0, 10);
                        logMessage(`Sending ${entries.length} entries`);
                        logMessage(entries);
                        logMessage(`Remaining entries: ${handle.remainingReadDirEntries.length}`);
                        sftpStream.name(reqid, entries);
                        return;
                    }

                    sftpStream.status(reqid, STATUS_CODE.EOF);
                });

                sftpStream.on('REMOVE', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('REMOVE', path);

                    const result = fakeFilesystem.find(path);
                    if (result === undefined) {
                        logMessage('File not found');
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }
                    if (result.type === 'directory') {
                        logMessage('Cannot remove directories');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    result.parent.removeChild(result.name);

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('RENAME', (reqid, oldPath, newPath) => {
                    oldPath = resolveFileName(oldPath);
                    newPath = resolveFileName(newPath);

                    logMessage('RENAME', oldPath, newPath);

                    if (renameFails()) {
                        logMessage('Fault: failing RENAME');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const oldResult = fakeFilesystem.find(oldPath);
                    if (oldResult === undefined) {
                        logMessage('Old file not found');
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }
                    const newResult = fakeFilesystem.find(newPath);
                    if (newResult !== undefined) {
                        logMessage('New file already exists');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    renameInFakeFs(oldResult, newPath);

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('EXTENDED', (reqid, name, data) => {
                    logMessage('EXTENDED', name);

                    if (!Object.hasOwn(advertisedExtensions, name))
                        return sftpStream.status(reqid, STATUS_CODE.OP_UNSUPPORTED);

                    const paths = readLengthPrefixedStrings(data, 2);
                    if (paths === undefined)
                        return sftpStream.status(reqid, STATUS_CODE.BAD_MESSAGE);
                    const oldPath = resolveFileName(paths[0]);
                    const newPath = resolveFileName(paths[1]);

                    const oldResult = fakeFilesystem.find(oldPath);
                    if (oldResult === undefined)
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    const newResult = fakeFilesystem.find(newPath);

                    if (name === 'posix-rename@openssh.com') {
                        if (renameFails()) {
                            logMessage('Fault: failing posix-rename');
                            return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                        }
                        if (newResult !== undefined) {
                            if (newResult.type === 'directory' && (oldResult.type !== 'directory' || newResult.children.length > 0))
                                return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                            newResult.parent.removeChild(newResult.name);
                        }
                        renameInFakeFs(oldResult, newPath);
                        return sftpStream.status(reqid, STATUS_CODE.OK);
                    }

                    if (name === 'hardlink@openssh.com') {
                        if (newResult !== undefined || oldResult.type !== 'file')
                            return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                        const parent = fakeFilesystem.find(pathUtil.dirname(newPath));
                        if (parent === undefined || parent.type !== 'directory')
                            return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                        parent.insert(file(pathUtil.basename(newPath), oldResult.content, oldResult.stat));
                        return sftpStream.status(reqid, STATUS_CODE.OK);
                    }

                    return sftpStream.status(reqid, STATUS_CODE.OP_UNSUPPORTED);
                });

                sftpStream.on('MKDIR', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('MKDIR', path);

                    const result = fakeFilesystem.find(path);
                    if (result !== undefined) {
                        logMessage('File already exists');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const parentPath = path.split('/').slice(0, -1).join('/');
                    const parent = fakeFilesystem.find(parentPath);
                    if (parent === undefined || parent.type !== 'directory') {
                        logMessage('Parent not found');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    parent.insert(directory({ name: pathUtil.basename(path) }));

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('RMDIR', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('RMDIR', path);

                    const result = fakeFilesystem.find(path);
                    if (result === undefined || result.type !== 'directory') {
                        logMessage('Not a directory');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    if (result.children.length > 0) {
                        logMessage('Directory not empty');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    result.parent.removeChild(result.name);

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('READLINK', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('READLINK', path);

                    const result = fakeFilesystem.find(path);
                    if (result === undefined || result.type !== 'symlink') {
                        logMessage('Not a symlink');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    // Dangling links are legal: return just the raw target literal even when
                    // nothing exists at the target path.
                    const target = result.target;
                    const find2 = fakeFilesystem.find(target);

                    if (find2 === undefined) {
                        logMessage('Target not found (dangling link) — returning literal anyway');
                        sftpStream.name(reqid, [{ filename: target, longname: target, attrs: {} }]);
                        return;
                    }

                    sftpStream.name(reqid, [{ filename: target, longname: makeLongName(find2), attrs: find2.stat }]);
                });

                // ssh2 emits SYMLINK arguments in OpenSSH wire order: (linkPath, target),
                // NOT (target, linkPath) as the standard SFTP v3 spec reads. libssh's
                // sftp_symlink sends in the OpenSSH order by default.
                sftpStream.on('SYMLINK', (reqid, linkPath, targetPath) => {
                    linkPath = resolveFileName(linkPath);
                    targetPath = resolveFileName(targetPath);

                    logMessage('SYMLINK link=', linkPath, 'target=', targetPath);

                    // Target is allowed to not exist — dangling links are a valid scenario.

                    const link = fakeFilesystem.find(linkPath);
                    if (link !== undefined) {
                        logMessage('Link already exists');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    };

                    // insertDeep(path, entry) inserts `entry` into the directory whose
                    // last path segment matches the final path component — i.e. pass the
                    // FULL link path (including the leaf name), not the parent directory.
                    const element = symlink(pathUtil.basename(linkPath), targetPath);
                    fakeFilesystem.insertDeep(linkPath, element);

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('SETSTAT', (reqid, path, attrs) => {
                    path = resolveFileName(path);

                    logMessage('SETSTAT', path, attrs);

                    const result = fakeFilesystem.find(path);
                    if (result === undefined) {
                        logMessage('File not found');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    // Apply fields selectively — ssh2 may surface a default 'size: 0' when the
                    // client didn't send ACMODSIZE, and blindly Object.assigning would truncate
                    // the visible file size to 0. Only copy the fields we actually care about.
                    if (attrs.atime !== undefined) result.stat.atime = attrs.atime;
                    if (attrs.mtime !== undefined) result.stat.mtime = attrs.mtime;
                    if (attrs.mode !== undefined) result.stat.mode = attrs.mode;
                    if (attrs.uid !== undefined) result.stat.uid = attrs.uid;
                    if (attrs.gid !== undefined) result.stat.gid = attrs.gid;

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('FSETSTAT', (reqid, handle, attrs) => {
                    logMessage('FSETSTAT', handle, attrs);

                    const handleString = handle.toString('utf8');
                    if (!handles.has(handleString)) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const handleObj = handles.get(handleString);
                    if (!handleObj) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const result = fakeFilesystem.find(handleObj.path);
                    if (result === undefined) {
                        logMessage('File not found').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    Object.assign(result.stat, attrs);

                    sftpStream.status(reqid, STATUS_CODE.OK);
                });

                sftpStream.on('FSTAT', (reqid, handle) => {
                    logMessage('FSTAT', handle);

                    const handleString = handle.toString('utf8');
                    if (!handles.has(handleString)) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }
                    const handleObj = handles.get(handleString);
                    if (!handleObj) {
                        logMessage('Invalid handle').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const result = fakeFilesystem.find(handleObj.path);
                    if (result === undefined) {
                        logMessage('File not found').trace();
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    sftpStream.attrs(reqid, result.stat);
                });

                sftpStream.on('LSTAT', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('LSTAT', path);

                    const result = fakeFilesystem.find(path);
                    if (result === undefined) {
                        logMessage('File not found');
                        return sftpStream.status(reqid, STATUS_CODE.NO_SUCH_FILE);
                    }

                    logMessage(result.stat);
                    sftpStream.attrs(reqid, result.stat);
                });
            });
        });
    });


    client.on('close', () => {
        logMessage('Client disconnected!');
    });
});

server.listen(port, '::', () => {
    const address = server.address();
    logMessage(`Listening on ${address.address}:${address.port}`);
    console.log(JSON.stringify({ port: server.address().port }));
})

server.on('error', (error) => {
    logMessage(`Server error: ${error}`);
    exit(1);
});

const cli = new CommandLineInterface(logMessage);

cli.on('macHome', () => {
    defaultPath = '/Users/test';
});
cli.on('realpathDotRelative', () => {
    realpathDotMode = 'relative';
});
cli.on('realpathDotEmpty', () => {
    realpathDotMode = 'empty';
});
cli.on('realpathDotFailure', () => {
    realpathDotMode = 'failure';
});
cli.on('removeHome', () => {
    fakeFilesystem.find('/home').removeChild('test');
});

const nullableBytes = (command) => (command.bytes === undefined || command.bytes === null) ? null : Number(command.bytes);
cli.on('failReadsAfter', (command) => {
    faults.failReadsAfter = nullableBytes(command);
    faults.bytesRead = 0;
});
cli.on('failWritesAfter', (command) => {
    faults.failWritesAfter = nullableBytes(command);
    faults.bytesWritten = 0;
});
cli.on('dropAfterBytes', (command) => {
    faults.dropAfterBytes = nullableBytes(command);
    faults.bytesRead = 0;
    faults.bytesWritten = 0;
});
cli.on('failRenames', (command) => {
    faults.failRenames = command.enabled !== false;
    faults.renamesBeforeFailure = command.after === undefined ? null : Number(command.after);
});
cli.on('denyDirectory', (command) => {
    faults.deniedDirectories.add(command.path);
});
// LSTAT of the path answers permission denied.
cli.on('denyStat', (command) => {
    faults.deniedStats.add(command.path);
});
cli.on('advertise', (command) => {
    advertisedExtensions[command.name] = command.data ?? '1';
});
// Inserts an entry under a raw name that may be invalid as a path segment, like '..' or 'a/b'.
cli.on('rawName', (command) => {
    const parent = fakeFilesystem.find(command.directory);
    if (parent === undefined || parent.type !== 'directory')
        return;
    if (command.type === 'directory')
        parent.insert(directory({ name: command.name }, []));
    else
        parent.insert(file(command.name, command.content ?? ''));
});
cli.on('createFile', (command) => {
    const parent = fakeFilesystem.find(pathUtil.posix.dirname(command.path));
    if (parent === undefined || parent.type !== 'directory')
        return;
    const name = pathUtil.posix.basename(command.path);
    parent.removeChild(name);
    const content = command.size !== undefined
        ? Buffer.alloc(Number(command.size), command.fill ?? 'x')
        : (command.content ?? '');
    parent.insert(file(name, content));
});
cli.on('createDirectory', (command) => {
    const parent = fakeFilesystem.find(pathUtil.posix.dirname(command.path));
    if (parent === undefined || parent.type !== 'directory')
        return;
    parent.insert(directory({ name: pathUtil.posix.basename(command.path) }, []));
});
cli.on('createSymlink', (command) => {
    const parent = fakeFilesystem.find(pathUtil.posix.dirname(command.path));
    if (parent === undefined || parent.type !== 'directory')
        return;
    parent.insert(symlink(pathUtil.posix.basename(command.path), command.target));
});

cli.on('exit', () => {
    logMessage('').end();
    server.close();
    // Exits even when a handle stays open, like stdin on Windows, which held every test until the kill timer.
    setTimeout(() => process.exit(0), 200).unref();
});