export class FlowValidationError extends Error {
  constructor(
    readonly path: string,
    message: string,
  ) {
    super(`${path}: ${message}`);
    this.name = "FlowValidationError";
  }
}

export class GraphImageError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "GraphImageError";
  }
}
