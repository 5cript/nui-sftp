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
                file('file1.txt', 'Fake file content'),
                file('file2.txt', 'Fake file content'),
                file('large.txt', (() => {
                    const size = 1024 * 1024;
                    const repeatable = 'abcdefghijklmnopqrstuvwxyz';
                    const division = size / repeatable.length;
                    const mod = size % repeatable.length;
                    return repeatable.repeat(division) + repeatable.slice(0, mod);
                })())
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

const defaultPath = '/home/test';

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
                sftpStream.on('OPEN', (reqid, filename, flags, attrs) => {
                    filename = resolveFileName(filename);

                    logMessage('OPEN', filename, flags, attrs);

                    const result = fakeFilesystem.find(filename);
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
                                result.content = '';
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

                    // Simulate writing data to the file at the specified offset
                    result.content = Buffer.concat([
                        Buffer.from(result.content.slice(0, offset)),
                        data,
                        Buffer.from(result.content.slice(offset + data.length))
                    ]).toString('utf8');
                    result.stat.size = result.content.length;

                    sftpStream.status(reqid, STATUS_CODE.OK);

                    logMessage(`Write to file at offset ${offset}: ${data.toString('utf8')}`);
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

                    const data = Buffer.from(result.content.slice(offset, Math.min(offset + length, size)), 'utf8');
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

                    if (result.type === 'symlink') {
                        const target = result.target;
                        const find2 = fakeFilesystem.find(target);

                        if (find2 === undefined) {
                            logMessage('Target not found');
                            return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                        }

                        sftpStream.attrs(reqid, find2.stat);
                    }

                    sftpStream.attrs(reqid, result.stat);
                });

                sftpStream.on('OPENDIR', (reqid, path) => {
                    path = resolveFileName(path);

                    logMessage('OPENDIR', path);

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

                    const target = result.target;
                    const find2 = fakeFilesystem.find(target);

                    if (find2 === undefined) {
                        logMessage('Target not found');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    sftpStream.name(reqid, [{ filename: target, longname: makeLongName(find2), attrs: find2.stat }]);

                    // sftpStream.name(reqid, [{ filename: path, longname: 'foo', attrs: {} }]);
                });

                sftpStream.on('SYMLINK', (reqid, targetPath, linkPath) => {
                    targetPath = resolveFileName(targetPath);
                    linkPath = resolveFileName(linkPath);

                    logMessage('SYMLINK', targetPath, linkPath);

                    const target = fakeFilesystem.find(targetPath);
                    if (target === undefined) {
                        // It is unclear whether the target must exist or not
                        logMessage('Target not found');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    }

                    const link = fakeFilesystem.find(linkPath);
                    if (link !== undefined) {
                        logMessage('Link already exists');
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
                    };

                    const element = symlink(pathUtil.filename(linkPath), targetPath);
                    fakeFilesystem.insertDeep(pathUtil.dirname(linkPath), element);

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

                    Object.assign(result.stat, attrs);

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
                        return sftpStream.status(reqid, STATUS_CODE.FAILURE);
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

cli.on('exit', () => {
    logMessage('').end();
    server.close();
});