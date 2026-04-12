import path from 'node:path';
import util from 'node:util';

const defaultStat = {
    mode: 0o644,
    uid: 1000,
    gid: 1000,
    userName: 'test',
    groupName: 'test',
    size: 0,
    atime: 0,
    mtime: 0,
    ctime: 0,
    birthtime: 0,
    linkCount: 0
}

const fdsProto = {
    path: function () {
        const parts = [];
        let current = this;
        while (current !== undefined) {
            parts.unshift(current.name);
            current = current.parent;
        }
        parts.filter(part => part.length > 0);
        return parts.join('/');
    }
}

// POSIX file-type bits. libssh parses the SFTPv3 permissions field to derive the entry's
// type — without these bits, a symlink would be reported as a regular file on the wire.
const S_IFREG = 0o100000;
const S_IFDIR = 0o040000;
const S_IFLNK = 0o120000;

// Like object spread, but drops keys whose value is undefined. Needed because ssh2 parses
// SFTP attrs into an object where unset fields are `undefined`, which would clobber our
// defaults (e.g. make `mtime` undefined and crash date formatting in makeLongName).
const defined = (obj) => {
    if (!obj) return {};
    const out = {};
    for (const k of Object.keys(obj)) if (obj[k] !== undefined) out[k] = obj[k];
    return out;
};

const file = (name, contentString, stat) => {
    const result = {
        type: 'file',
        name: name,
        stat: {
            ...defaultStat,
            ...defined(stat),
            mode: ((stat && stat.mode) || 0o644) | S_IFREG,
        },
        content: contentString || '',
    };
    result.stat.size = contentString ? contentString.length : 0;
    Object.setPrototypeOf(result, fdsProto);
    return result;
}

const directory = ({ name, stat }, children) => {
    const result = {
        root: name === '',
        type: 'directory',
        name: name,
        stat: {
            ...defaultStat,
            ...defined(stat),
            mode: 0o755 | S_IFDIR
        },
        children: children || [],
        insert: function (entry) {
            entry.parent = this;
            this.children.push(entry);
        },
        insertDeep: function (pathString, entry) {
            if (pathString === '/') {
                this.children.push(entry);
                return;
            }

            const pathParts = pathString.split('/').filter(part => part.length > 0);
            const currentSegment = pathParts[0];
            const remainingPath = pathParts.slice(1).join('/');

            if (pathParts.length === 1) {
                entry.parent = this;
                this.children.push(entry);
                return;
            }

            const existingChild = this.children.find(child => child.name === currentSegment);
            if (existingChild === undefined) {
                const newDir = directory({ name: currentSegment }, []);
                this.children.push(newDir);
                newDir.insertDeep(remainingPath, entry);
            } else {
                existingChild.insertDeep(remainingPath, entry);
            }
        },
        find: function (pathString) {
            if (pathString === '/' && this.root)
                return this;

            const pathParts = pathString.split('/').filter(part => part.length > 0);

            if (pathString === '')
                return this;

            if (pathParts.length !== 0) {
                const currentSegment = pathParts[0];
                if (currentSegment === '..') {
                    if (this.parent === undefined) {
                        return undefined;
                    }
                    const remainingPath = pathParts.slice(1).join('/');
                    return this.parent.find(remainingPath);
                }
                if (currentSegment === '.') {
                    const remainingPath = pathParts.slice(1).join('/');
                    return this.find(remainingPath);
                }
                const child = this.children.find(child => child.name === currentSegment);

                if (child === undefined)
                    return undefined;

                const remainingPath = pathParts.slice(1).join('/');
                if (remainingPath === '')
                    return child;

                return child.find(remainingPath);
            }
        },
        removeChild: function (name) {
            const index = this.children.findIndex(child => child.name === name);
            if (index !== -1) {
                this.children.splice(index, 1);
            }
        }
    };
    Object.setPrototypeOf(result, fdsProto);
    return result;
}

const symlink = (name, target, stat) => {
    if (stat === undefined)
        stat = {}

    const result = {
        type: 'symlink',
        name: name,
        target: target,
        stat: {
            ...defaultStat,
            ...defined(stat),
            mode: 0o777 | S_IFLNK
        }
    }
    Object.setPrototypeOf(result, fdsProto);
    return result;
}

// Gives every child directory a reference to its parent directory
const fillFakeFsParents = (root) => {
    const fillParent = (parent, child) => {
        child.parent = parent;
        if (child.type === 'directory') {
            child.children.forEach(c => fillParent(child, c));
        }
    }

    fillParent(undefined, root);
}

const finalizeFakeFs = (root) => {
    fillFakeFsParents(root);
    return root;
}

const renameInFakeFs = (oldEntry, newPath) => {
    let root = oldEntry.parent;
    if (root.parent === undefined) {
        root = oldEntry;
    }
    else {
        while (root.parent !== undefined) {
            root = root.parent;
        }
    }

    const newEntry = { ...oldEntry };
    Object.setPrototypeOf(newEntry, fdsProto);
    newEntry.parent = undefined;
    newEntry.name = path.basename(newPath);

    oldEntry.parent.removeChild(oldEntry.name);
    root.insertDeep(newPath, newEntry);
}

export { file, directory, symlink, finalizeFakeFs, renameInFakeFs };