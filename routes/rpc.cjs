import { verifyUser } from "../middleware/auth.cjs";
import { findUserInDB } from "../services/db_service.cjs";
import logger from "../utils/logger.cjs";

export async function handleRPCRequest(data, mempool) {
    const { method, params, id } = data;

    switch (method) {
        case "medor_login":
            // Directly calls your verifyUser(username, password, dbLookupFunc)
            const token = await verifyUser(params[0], params[1], findUserInDB);
            return { token, status: "SUCCESS" };

        case "sendRawTransaction":
            // Sends the transaction to your mempool.cjs
            return await mempool.addTransaction(params[0]);

        case "getMiningInfo":
            return mempool.getMiningTemplate();

        default:
            logger.warn("RPC", `Unknown method: ${method}`);
            throw new Error("Method not found");
    }
}
