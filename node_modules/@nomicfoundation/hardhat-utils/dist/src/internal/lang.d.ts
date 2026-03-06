export declare function getDeepCloneFunction(): Promise<(<T>(input: T) => T)>;
export declare function deepMergeImpl<T extends object, S extends object>(target: T, source: S, shouldOverwriteUndefined: boolean): T & S;
/**
 * Performs a custom deep equality check using `fast-equals` with specific overrides.
 *
 * @param x The first value to compare.
 * @param y The second value to compare.
 * @returns A promise that resolves to true if the values are deeply equal, false otherwise.
 */
export declare function customFastEqual<T>(x: T, y: T): Promise<boolean>;
//# sourceMappingURL=lang.d.ts.map