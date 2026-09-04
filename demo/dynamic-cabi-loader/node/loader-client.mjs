import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { pathToFileURL } from "node:url";

export class LoaderClient {
  constructor(python, server, manifest) {
    this.process = spawn(python, [server, manifest], { stdio: ["pipe", "pipe", "inherit"] });
    this.lines = createInterface({ input: this.process.stdout });
    this.pending = [];
    this.lines.on("line", line => this.pending.shift()?.(JSON.parse(line)));
  }

  resolve(module) {
    return new Promise((resolve, reject) => {
      this.pending.push(message => message.ok ? resolve(message.result) : reject(new Error(message.error)));
      this.process.stdin.write(`${JSON.stringify({ module })}\n`);
    });
  }

  async import(module) {
    const result = await this.resolve(module);
    return { metadata: result, namespace: await import(pathToFileURL(result.script).href) };
  }

  close() { this.process.stdin.end(); }
}

