// Chrome DevTools protocol client for a CEF build of nui-sftp started with NUI_CEF_REMOTE_DEBUGGING_PORT.
// Keeps one connection to the app page open for the whole run.
import { writeFileSync } from 'node:fs';

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

const findPageTarget = async (port) => {
    const targets = await fetch(`http://127.0.0.1:${port}/json/list`)
        .then((response) => response.json())
        .catch(() => null);
    return targets?.find((target) => target.type === 'page' && !target.url.startsWith('devtools://')) ?? null;
};

export class DevTools {
    constructor(socket) {
        this.socket_ = socket;
        this.nextId_ = 1;
        this.pending_ = new Map();
        socket.onmessage = (event) => {
            const message = JSON.parse(event.data);
            const resolve = this.pending_.get(message.id);
            if (resolve) {
                this.pending_.delete(message.id);
                resolve(message);
            }
        };
    }

    /**
     * @brief Connects to the app page, retrying until it is listed or the timeout ends.
     * @param port The remote debugging port.
     * @param timeoutMilliseconds How long to wait for the page target.
     */
    static async connect(port, timeoutMilliseconds = 60000) {
        const deadline = Date.now() + timeoutMilliseconds;
        let target = null;
        while (!target) {
            if (Date.now() > deadline)
                throw new Error(`no page target on port ${port}`);
            target = await findPageTarget(port);
            if (!target)
                await sleep(500);
        }
        const socket = new WebSocket(target.webSocketDebuggerUrl);
        await new Promise((resolve, reject) => {
            socket.onopen = resolve;
            socket.onerror = () => reject(new Error(`cannot open the DevTools socket on port ${port}`));
        });
        return new DevTools(socket);
    }

    /**
     * @brief Sends a raw protocol command and returns its result, throws on a protocol error.
     */
    async send(method, parameters = {}, timeoutMilliseconds = 60000) {
        const id = this.nextId_++;
        const reply = new Promise((resolve) => this.pending_.set(id, resolve));
        this.socket_.send(JSON.stringify({ id, method, params: parameters }));
        let timer = null;
        const timeout = new Promise((_, reject) => {
            timer = setTimeout(() => {
                this.pending_.delete(id);
                reject(new Error(`${method} got no reply within ${timeoutMilliseconds} ms`));
            }, timeoutMilliseconds);
        });
        try {
            const message = await Promise.race([reply, timeout]);
            if (message.error)
                throw new Error(`${method}: ${message.error.message}`);
            return message.result;
        } finally {
            clearTimeout(timer);
        }
    }

    /**
     * @brief Evaluates an expression in the page, awaits promises and returns the value as JSON.
     * @param expression A JavaScript expression. Statements need an IIFE around them.
     */
    async evaluate(expression, timeoutMilliseconds = 120000) {
        const result = await this.send(
            'Runtime.evaluate',
            { expression, awaitPromise: true, returnByValue: true },
            timeoutMilliseconds
        );
        if (result.exceptionDetails) {
            const details = result.exceptionDetails;
            throw new Error(details.exception?.description ?? details.text ?? 'evaluation failed');
        }
        return result.result?.value;
    }

    /**
     * @brief Calls globalThis[objectName][functionName] in the page with JSON-serializable arguments.
     */
    call(objectName, functionName, ...parameters) {
        const argumentList = parameters.map((parameter) => JSON.stringify(parameter)).join(', ');
        return this.evaluate(`globalThis.${objectName}.${functionName}(${argumentList})`);
    }

    /**
     * @brief Writes a PNG of the current viewport.
     */
    async screenshot(path) {
        const result = await this.send('Page.captureScreenshot', { format: 'png' });
        writeFileSync(path, Buffer.from(result.data, 'base64'));
    }

    close() {
        this.socket_.close();
    }
}
