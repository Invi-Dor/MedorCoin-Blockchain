#pragma once  // Prevents multiple inclusion automatically

// or alternatively, if you prefer classic include guards
/*
#ifndef BLOCKDB_H
#define BLOCKDB_H
*/

#include <string>
#include <vector>
// include other headers you actually need here
//#include "otherheader.h"   // only include other dependencies, NOT itself

// your class or struct definitions
class BlockDB {
public:
    BlockDB();
    ~BlockDB();

    void addBlock(const std::string& data);
    std::string getBlock(int index);

private:
    std::vector<std::string> blocks;
};

/*
#endif // BLOCKDB_H
*/
