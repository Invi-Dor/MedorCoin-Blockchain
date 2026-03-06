export declare const FIRST_OFFICIAL_ARM64_SOLC_VERSION = "0.8.31";
/**
 * Determines if a solc version has an official ARM64 Linux build.
 */
export declare function hasOfficialArm64Build(version: string): boolean;
/**
 * Returns true if running on a platform that doesn't have official native
 * solc builds for all versions (currently ARM64 Linux before 0.8.31).
 */
export declare function missesSomeOfficialNativeBuilds(): boolean;
export declare function getEvmVersionFromSolcVersion(solcVersion: string): string | undefined;
//# sourceMappingURL=solc-info.d.ts.map