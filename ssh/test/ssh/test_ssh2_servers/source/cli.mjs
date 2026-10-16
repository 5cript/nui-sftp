import readline from 'node:readline';

class CommandLineInterface {
    constructor(logMessage) {
        this.rl = readline.createInterface({
            input: process.stdin,
            output: process.stdout
        });

        this.eventMap = new Map();

        logMessage('Command line interface started');

        this.rl.on('line', (input) => {
            logMessage(`Command from stdin: ${input}`);
            if (input.length === 0)
                return;

            const command = JSON.parse(input);
            if (Object.hasOwnProperty.call(command, 'command')) {
                // Exit runs its handler once and closes stdin.
                if (command.command === 'exit') {
                    if (this.eventMap.has('exit')) {
                        this.eventMap.get('exit')();
                    }
                    this.close();
                    return;
                }

                if (this.eventMap.has(command.command)) {
                    this.eventMap.get(command.command)(command);
                }
                if (command.ack === true) {
                    process.stdout.write(JSON.stringify({ ack: command.command }) + '\n');
                }
            }
        });
    }

    on(event, callback) {
        this.eventMap.set(event, callback);
    }

    close() {
        this.rl.close();
    }
};

export default CommandLineInterface;