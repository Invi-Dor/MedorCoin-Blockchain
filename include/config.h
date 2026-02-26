#ifndef MEDORCOIN_CONFIG_H
#define MEDORCOIN_CONFIG_H

// MedorCoin configuration values

// Enable or disable debug logging (0 = off, 1 = on)
#ifndef MEDORCOIN_DEBUG
#define MEDORCOIN_DEBUG 0
#endif

// Maximum number of transactions per block
#ifndef MEDORCOIN_MAX_TRANSACTIONS
#define MEDORCOIN_MAX_TRANSACTIONS 500
#endif

// Mining difficulty default (can be modified later)
#ifndef MEDORCOIN_DEFAULT_DIFFICULTY
#define MEDORCOIN_DEFAULT_DIFFICULTY 1
#endif

#endif // MEDORCOIN_CONFIG_H
