import type { Compiler } from "../../../../../types/solidity.js";
export declare function downloadSolcCompilers(versions: Set<string>, quiet: boolean): Promise<void>;
export declare function getCompiler(version: string, { preferWasm, compilerPath }: {
    preferWasm: boolean;
    compilerPath?: string;
}): Promise<Compiler>;
//# sourceMappingURL=index.d.ts.map