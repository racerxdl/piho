import { FlowValidationError } from "./errors.ts";

type JsonObject = Record<string, unknown>;

class StrictJsonParser {
  private index = 0;

  constructor(private readonly source: string) {}

  parse(): unknown {
    this.skipWhitespace();
    const value = this.parseValue("$");
    this.skipWhitespace();
    if (this.index !== this.source.length) {
      this.syntaxError("unexpected trailing content");
    }
    return value;
  }

  private syntaxError(message: string): never {
    throw new FlowValidationError("$", `invalid JSON at offset ${this.index}: ${message}`);
  }

  private skipWhitespace(): void {
    while (this.index < this.source.length) {
      const character = this.source.charCodeAt(this.index);
      if (character !== 0x20 && character !== 0x09 && character !== 0x0a && character !== 0x0d) {
        return;
      }
      this.index += 1;
    }
  }

  private parseValue(path: string): unknown {
    const character = this.source[this.index];
    switch (character) {
      case "{":
        return this.parseObject(path);
      case "[":
        return this.parseArray(path);
      case '"':
        return this.parseString();
      case "t":
        return this.parseLiteral("true", true);
      case "f":
        return this.parseLiteral("false", false);
      case "n":
        return this.parseLiteral("null", null);
      default:
        return this.parseNumber();
    }
  }

  private parseObject(path: string): JsonObject {
    this.index += 1;
    this.skipWhitespace();
    const object = Object.create(null) as JsonObject;
    const keys = new Set<string>();
    if (this.source[this.index] === "}") {
      this.index += 1;
      return object;
    }

    while (this.index < this.source.length) {
      if (this.source[this.index] !== '"') {
        this.syntaxError("expected an object key");
      }
      const key = this.parseString();
      if (keys.has(key)) {
        throw new FlowValidationError(`${path}.${key}`, "is a duplicate JSON key");
      }
      keys.add(key);
      this.skipWhitespace();
      if (this.source[this.index] !== ":") {
        this.syntaxError("expected ':' after an object key");
      }
      this.index += 1;
      this.skipWhitespace();
      object[key] = this.parseValue(`${path}.${key}`);
      this.skipWhitespace();
      const delimiter = this.source[this.index];
      if (delimiter === "}") {
        this.index += 1;
        return object;
      }
      if (delimiter !== ",") {
        this.syntaxError("expected ',' or '}' in an object");
      }
      this.index += 1;
      this.skipWhitespace();
    }
    return this.syntaxError("unterminated object");
  }

  private parseArray(path: string): unknown[] {
    this.index += 1;
    this.skipWhitespace();
    const values: unknown[] = [];
    if (this.source[this.index] === "]") {
      this.index += 1;
      return values;
    }

    while (this.index < this.source.length) {
      values.push(this.parseValue(`${path}[${values.length}]`));
      this.skipWhitespace();
      const delimiter = this.source[this.index];
      if (delimiter === "]") {
        this.index += 1;
        return values;
      }
      if (delimiter !== ",") {
        this.syntaxError("expected ',' or ']' in an array");
      }
      this.index += 1;
      this.skipWhitespace();
    }
    return this.syntaxError("unterminated array");
  }

  private parseString(): string {
    const start = this.index;
    this.index += 1;
    while (this.index < this.source.length) {
      const character = this.source.charCodeAt(this.index);
      if (character === 0x22) {
        this.index += 1;
        try {
          return JSON.parse(this.source.slice(start, this.index)) as string;
        } catch {
          return this.syntaxError("invalid string escape");
        }
      }
      if (character < 0x20) {
        this.syntaxError("unescaped control character in string");
      }
      if (character === 0x5c) {
        this.index += 1;
        const escape = this.source[this.index];
        if (escape === "u") {
          const digits = this.source.slice(this.index + 1, this.index + 5);
          if (!/^[0-9A-Fa-f]{4}$/.test(digits)) {
            this.syntaxError("invalid Unicode escape");
          }
          this.index += 5;
          continue;
        }
        if (escape === undefined || !['"', "\\", "/", "b", "f", "n", "r", "t"].includes(escape)) {
          this.syntaxError("invalid string escape");
        }
      }
      this.index += 1;
    }
    return this.syntaxError("unterminated string");
  }

  private parseLiteral<T>(token: string, value: T): T {
    if (!this.source.startsWith(token, this.index)) {
      this.syntaxError(`expected ${token}`);
    }
    this.index += token.length;
    return value;
  }

  private parseNumber(): number {
    const match = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/.exec(
      this.source.slice(this.index),
    );
    if (match === null) {
      return this.syntaxError("expected a JSON value");
    }
    this.index += match[0].length;
    return Number(match[0]);
  }
}

export function parseStrictJson(source: string): unknown {
  return new StrictJsonParser(source).parse();
}
