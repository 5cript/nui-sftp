// Conformance of the OSC 633 path through the real xterm parser, headless in node (no DOM is needed
// as long as the terminal is never opened). Run with: npm run test:osc
//
// The pty tests (backend/test/backend/test_shell_integration.hpp) prove what the shells emit, using a
// small C++ test double instead of xterm. These tests prove that xterm turns those bytes into the
// payloads the double reported:
// - the synthetic cases pin down the xterm behaviours the double mirrors;
// - with NUI_SFTP_PTY_FIXTURES pointing at the fixture directory the pty tests write
//   (<build>/test/temp/shell_integration/fixtures), every recorded shell session is replayed
//   through xterm in random chunks and must yield exactly the payloads the double found.

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import xterm from "@xterm/xterm";

const { Terminal } = xterm;
const oscCode = 633;
const encoder = new TextEncoder();

function createTerminal() {
    const terminal = new Terminal({ allowProposedApi: true, cols: 200, rows: 30 });
    const payloads = [];
    // Same registration as terminalUtility.registerOscHandler in terminal_channel.cpp.
    terminal.parser.registerOscHandler(oscCode, (payload) => {
        payloads.push(payload);
        return true;
    });
    return { terminal, payloads };
}

function writeChunks(terminal, chunks) {
    return new Promise((resolve) => {
        for (const chunk of chunks)
            terminal.write(chunk);
        terminal.write("", resolve);
    });
}

async function payloadsFor(chunks) {
    const { terminal, payloads } = createTerminal();
    await writeChunks(terminal, chunks);
    terminal.dispose();
    return payloads;
}

function screenText(terminal) {
    const buffer = terminal.buffer.active;
    const lines = [];
    for (let row = 0; row < buffer.length; ++row)
        lines.push(buffer.getLine(row).translateToString(true));
    return lines.join("\n").trimEnd();
}

function splitBytes(bytes, sizes) {
    const chunks = [];
    let offset = 0;
    let index = 0;
    while (offset < bytes.length) {
        const size = sizes[index++ % sizes.length];
        chunks.push(bytes.subarray(offset, offset + size));
        offset += size;
    }
    return chunks;
}

test("a sequence fires once with its exact payload", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]633;E;git status\x07"]), ["E;git status"]);
});

test("ESC backslash terminates like BEL", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]633;E;ls\x1b\\"]), ["E;ls"]);
});

test("a sequence split at any position still fires once", async () => {
    const sequence = "pre\x1b]633;E;echo split\x07post";
    for (let split = 1; split < sequence.length; ++split) {
        assert.deepEqual(
            await payloadsFor([sequence.slice(0, split), sequence.slice(split)]),
            ["E;echo split"],
            `split at ${split}`
        );
    }
    assert.deepEqual(await payloadsFor([...sequence]), ["E;echo split"], "one character per write");
});

test("multibyte characters split across writes survive", async () => {
    const bytes = encoder.encode("\x1b]633;E;echo ü 日 \u{1F600}\x07");
    for (const size of [1, 2, 3, 5])
        assert.deepEqual(await payloadsFor(splitBytes(bytes, [size])), ["E;echo ü 日 \u{1F600}"], `${size} bytes`);
});

test("the sequence is swallowed and never reaches the screen", async () => {
    const { terminal } = createTerminal();
    await writeChunks(terminal, ["before\x1b]633;E;secret command\x07after"]);
    assert.equal(screenText(terminal), "beforeafter");
    terminal.dispose();
});

test("an unrelated OSC does not fire the handler", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]0;window title\x07\x1b]6330;E;no\x07\x1b]63;E;no\x07"]), []);
});

test("escaped payloads arrive verbatim, decoding is ours", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]633;E;echo a\\\\b\\x3b\\x0a\x07"]), ["E;echo a\\\\b\\x3b\\x0a"]);
});

// The behaviours the C++ OscScanner double mirrors; if one changes, the double must change with it.
test("C0 controls inside the string are dropped (why the hooks escape them)", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]633;E;a\nb\tc\rd\x07"]), ["E;abcd"]);
});

test("ESC inside the string ends it", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]633;E;ab\x1b[31mcd\x07"]), ["E;ab"]);
});

test("CAN and SUB abort the string", async () => {
    assert.deepEqual(await payloadsFor(["\x1b]633;E;ab\x18cd\x07", "\x1b]633;E;ef\x1agh\x07"]), []);
});

const fixtureDirectory = process.env.NUI_SFTP_PTY_FIXTURES;

test("recorded shell sessions yield the payloads the test double found", { skip: !fixtureDirectory && "NUI_SFTP_PTY_FIXTURES is not set" }, async () => {
    assert.ok(existsSync(fixtureDirectory), `${fixtureDirectory} does not exist; run the pty tests first`);
    const sessions = readdirSync(fixtureDirectory).filter((name) => name.endsWith(".bin"));
    assert.ok(sessions.length > 0, "no recorded sessions");

    for (const session of sessions) {
        const bytes = new Uint8Array(readFileSync(join(fixtureDirectory, session)));
        const expected = JSON.parse(readFileSync(join(fixtureDirectory, session.replace(/\.bin$/, ".payloads.json")), "utf8"));
        assert.deepEqual(await payloadsFor([bytes]), expected, `${session} in one piece`);
        assert.deepEqual(await payloadsFor(splitBytes(bytes, [1, 7, 3, 64, 2, 509])), expected, `${session} in odd chunks`);
    }
});
