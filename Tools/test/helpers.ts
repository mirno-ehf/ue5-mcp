/**
 * helpers.ts — HTTP wrappers and fixture management for integration tests.
 */

import { TEST_BASE_URL } from "./bootstrap.js";

// ---------------------------------------------------------------------------
// HTTP helpers (mirror the production ueGet/uePost in src/index.ts)
// ---------------------------------------------------------------------------

export async function ueGet(
  endpoint: string,
  params: Record<string, string> = {},
): Promise<any> {
  const url = new URL(endpoint, TEST_BASE_URL);
  for (const [k, v] of Object.entries(params)) {
    if (v) url.searchParams.set(k, v);
  }
  const resp = await fetch(url.toString(), {
    signal: AbortSignal.timeout(30_000),
  });
  return resp.json();
}

export async function uePost(
  endpoint: string,
  body: Record<string, any>,
): Promise<any> {
  const resp = await fetch(`${TEST_BASE_URL}${endpoint}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(30_000),
  });
  return resp.json();
}

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

/** Generate a unique Blueprint name using a prefix and timestamp. */
export function uniqueName(prefix: string): string {
  return `${prefix}_${Date.now()}`;
}

/**
 * Create a test Blueprint via the HTTP API.
 * Returns the full JSON response from /api/create-blueprint.
 */
export async function createTestBlueprint(opts: {
  name: string;
  packagePath?: string;
  parentClass?: string;
  blueprintType?: string;
}): Promise<any> {
  return uePost("/api/create-blueprint", {
    blueprintName: opts.name,
    packagePath: opts.packagePath ?? "/Game/Test",
    parentClass: opts.parentClass ?? "Actor",
    blueprintType: opts.blueprintType ?? "Normal",
  });
}

/**
 * Delete a test Blueprint via the HTTP API (force = true to skip reference checks).
 * Returns the full JSON response from /api/delete-asset.
 */
export async function deleteTestBlueprint(
  assetPath: string,
): Promise<any> {
  return uePost("/api/delete-asset", {
    assetPath,
    force: true,
  });
}

/**
 * Create a test Material via the HTTP API.
 */
export async function createTestMaterial(opts: {
  name: string;
  packagePath?: string;
  domain?: string;
  blendMode?: string;
}): Promise<any> {
  return uePost("/api/create-material", {
    name: opts.name,
    packagePath: opts.packagePath ?? "/Game/Test",
    domain: opts.domain ?? "Surface",
    blendMode: opts.blendMode ?? "Opaque",
  });
}

/**
 * Delete a test Material via the HTTP API.
 */
export async function deleteTestMaterial(assetPath: string): Promise<any> {
  return uePost("/api/delete-asset", {
    assetPath,
    force: true,
  });
}

/**
 * Create a test Material Instance via the HTTP API.
 */
export async function createTestMaterialInstance(opts: {
  name: string;
  packagePath?: string;
  parentMaterial: string;
}): Promise<any> {
  return uePost("/api/create-material-instance", {
    name: opts.name,
    packagePath: opts.packagePath ?? "/Game/Test",
    parentMaterial: opts.parentMaterial,
  });
}


/**
 * Create a test Material Function via the HTTP API.
 */
export async function createTestMaterialFunction(opts: {
  name: string;
  packagePath?: string;
  description?: string;
}): Promise<any> {
  return uePost("/api/create-material-function", {
    name: opts.name,
    packagePath: opts.packagePath ?? "/Game/Test",
    description: opts.description ?? "",
  });
}

/**
 * Create a test Animation Blueprint via the HTTP API.
 */
export async function createTestAnimBlueprint(opts: {
  name: string;
  packagePath?: string;
  skeleton?: string;
  parentClass?: string;
}): Promise<any> {
  return uePost("/api/create-anim-blueprint", {
    name: opts.name,
    packagePath: opts.packagePath ?? "/Game/Test",
    skeleton: opts.skeleton ?? "__create_test_skeleton__",
    parentClass: opts.parentClass ?? "AnimInstance",
  });
}
