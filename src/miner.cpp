// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"
#ifdef ENABLE_MINING
#include "pow/tromp/equi_miner.h"
#endif

#include "amount.h"
#include "base58.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#ifdef ENABLE_MINING
#include "crypto/equihash.h"
#endif
#include "hash.h"
#include "main.h"
#include "metrics.h"
#include "net.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "random.h"
#include "timedata.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"

#include "flat-database.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include "sodium.h"

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#ifdef ENABLE_MINING
#include <functional>
#endif
#include <mutex>

#include <fstream>

#include <univalue.h>
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "keystore.h"
#include "rpcrawtransaction.cpp"
#include "merkleblock.h"
#include "rpcserver.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "script/standard.h"
#include "uint256.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.
// The COrphan class keeps track of these 'temporary orphans' while
// CreateBlock is figuring out which transactions to include.
//
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), dPriority(0)
    {
    }
};

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

// We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) {}

    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee) {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        } else {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

void UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    pblock->nTime = std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
}
//Called Every New Fork Block
CBlockTemplate* CreateNewForkBlock(bool& bFileNotFound)
{
    /*
      Because CreateNewForkBlock does file io when
      reading utxos, we grab the main lock to peek
      at the tip, release it to read the file and
      fill in the template and then reacquire it
      at the end to check if the active tip changed
      while we were doing the above
     */

    CBlockTemplate* ret;
    int tipHeight;
    int snappedHeight;
    const CChainParams& chainparams = Params();

    {
        LOCK(cs_main);
        tipHeight = chainActive.Tip()->nHeight;
    }

    do {
        snappedHeight = tipHeight;
        ret = CreateNewForkBlock(bFileNotFound, snappedHeight + 1);

        {
            LOCK(cs_main);
            CBlockIndex* pindexPrev = chainActive.Tip();
            CBlock* pblock = &ret->block; // pointer for convenience

            tipHeight = pindexPrev->nHeight;
            if (tipHeight != snappedHeight) {
                LogPrintf("WARN: tip changed from %u to %u while generating block template\n",
                          snappedHeight, tipHeight);
                continue;
            }

            // if we are good then fill in the final details
            pblock->hashPrevBlock = pindexPrev->GetBlockHash();
            UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
            pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
            pblock->nVersion = ComputeBlockVersion(pindexPrev, Params().GetConsensus());

            CValidationState state;
            if (!TestBlockValidity(state, *pblock, pindexPrev, false, false, true))
                throw std::runtime_error("CreateNewForkBlock(): TestBlockValidity failed\n");
        }
    } while (snappedHeight != tipHeight);

    return ret;
}

CBlockTemplate* CreateNewForkBlock(bool& bFileNotFound, const int nHeight)
{
    const CChainParams& chainparams = Params();

    const int nForkHeight = nHeight - chainparams.ForkStartHeight();
    const int Z_UTXO_MINING_START_BlOCK = chainparams.ZUtxoMiningStartBlock();


    //Here is the UTXO directory, which file we will read from
    string utxo_file_path = GetUTXOFileName(nHeight);
    LogPrintf("utxo_file_path: %s \n", utxo_file_path);
    //Read from the specified UTXO file
    std::ifstream if_utxo(utxo_file_path, std::ios::binary | std::ios::in);
    if (!if_utxo.is_open()) {
        bFileNotFound = true;
        LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: Cannot open UTXO file - %s\n",
                  nHeight, nForkHeight, forkHeightRange, utxo_file_path);
        return NULL;
    }


    // Create new block
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if (!pblocktemplate.get())
        return NULL;
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience
  

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = (unsigned int)(MAX_BLOCK_SIZE - 1000);

    uint64_t nBlockTotalAmount = 0;
    uint64_t nBlockSize = 1000;
    uint64_t nBlockTx = 0;
    uint64_t nBlockSigOps = 100;
    //while utxo files exists, and the number of tx in the block is less than set man (where is forkCBPerBlock)

  // Add dummy coinbase tx as first transaction
    CMutableTransaction txCoinbase;
    txCoinbase.vin.resize(1);
    txCoinbase.vin[0].prevout.SetNull();
    txCoinbase.vout.resize(1);
    txCoinbase.vout[0].nValue = 0;

    std::unique_ptr<char[]> pubKeyScript(new char[64]);
    unsigned char* pks = (unsigned char*)pubKeyScript.get();
    txCoinbase.vout[0].scriptPubKey = CScript(pks, pks+64);
    txCoinbase.vin[0].scriptSig = CScript() << nHeight << CScriptNum(nBlockTx) << ToByteVector(hashPid) << OP_0;

    pblock->vtx.push_back(txCoinbase);
    pblocktemplate->vTxFees.push_back(-1);   // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); 

    LogPrintf("Size of the block: %d \n", pblock->vtx.size());
    //START MINING Z-ADDRESSES
    // if (nHeight >= Z_UTXO_MINING_START_BlOCK) {
    LogPrintf("Z_UTXO_MINING_START_BlOCK: %d \n", Z_UTXO_MINING_START_BlOCK);
        if(nHeight == Z_UTXO_MINING_START_BlOCK){
            LogPrintf("INSIDE Z_UTXO_MINING_START_BlOCK: \n");
            while (true) {  
                LogPrintf("1@: \n");
                //break if there are no more transactions in the file
                if(if_utxo.eof()){
                    break;
                }

                CTransaction *txNew = new CTransaction();
                
                char* transSize = new char[32];
                
                //retrieve transaction size
                if (!if_utxo.read(transSize, 32)) {
                    LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? - Coudn't read the transaction size\n",
                            nHeight, nForkHeight, forkHeightRange);
                    break;
                }

                //convert binary size to int size
                char* endptr;
                int size = strtol(transSize, &endptr, 2);
                
                LogPrintf("UTXO-SIZE: %d\n", size);
                if (size == 0) {
                    LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: End of UTXO file ? - Transaction size is zero\n",
                            nHeight, nForkHeight, forkHeightRange);
                    break;
                }

                //load transaction (binary)
                LogPrintf("Size is: %d\n", size);
                char *rawTransaction = new char[size];
                if (!if_utxo.read(rawTransaction, size)) {
                    LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? - Coudn't read the transaction\n", nHeight, nForkHeight, forkHeightRange);
                    break;
                } 
                
                //converting binary raw transaction to hex-string raw transaction  10111011111010 => 2EFA
                std::stringstream ss;
                ss << std::hex << std::setfill('0');
                for (int i = 0; i < size; ++i)
                {
                    ss << std::setw(2) << (unsigned int)(unsigned char)(rawTransaction[i]);
                }
                LogPrintf("Size of the 1st transaction: %d\n", size);
                std::string rawTransactionHex = ss.str();
                LogPrintf("Transaction in hex: %s\n", rawTransactionHex);
        
                UniValue hexString = UniValue(rawTransactionHex);
                // LogPrintf("%s", rawTransactionHex);
        
                LogPrintf("BEFORE DECODE\n");
                decoderawtransaction2(*txNew, hexString, false);
                LogPrintf("1\n");
                // assert(0);
                        
                CMutableTransaction *txM = new CMutableTransaction(*txNew);

                // Add coinbase tx's
                // CMutableTransaction txNew;
                txM->vin.resize(1);
                //No input cuz coinbase
                txM->vin[0].prevout.SetNull();
                //Just create
                txM->vout.resize(1);
                txM->vout[0].nValue = 0;
                // *txNew->vout[0].scriptPubKey = CScript(pks, pks + pbsize);
                LogPrintf("2\n");
            
                unsigned int nTxSize = ::GetSerializeSize(*txM, SER_NETWORK, PROTOCOL_VERSION);
                if (nBlockSize + nTxSize >= nBlockMaxSize) {
                    LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: %u: block would exceed max size\n",
                            nHeight, nForkHeight, forkHeightRange, nBlockTx);
                    break;
                }

                // Legacy limits on sigOps:
                unsigned int nTxSigOps = GetLegacySigOpCount(*txM);
                if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS) {
                    LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: %u: block would exceed max sigops\n",
                            nHeight, nForkHeight, forkHeightRange, nBlockTx);
                    break;
                }

                pblock->vtx.push_back(*txM);
                pblocktemplate->vTxFees.push_back(0);
                pblocktemplate->vTxSigOps.push_back(nTxSigOps);
                nBlockSize += nTxSize;
                nBlockSigOps += nTxSigOps;
                ++nBlockTx;

                delete txNew;
                delete txM;
                delete transSize;
                delete rawTransaction;
            }
    } else {

        while (if_utxo && nBlockTx < forkCBPerBlock) {
        char term = 0;
////////////////////////Format checks, explore more when looking at UTXO raw
        //Value
        char coin[8] = {};
        if (!if_utxo.read(coin, 8)) {
            // the last file may be shorter than forkCBPerBlock
            if(!if_utxo.eof() || nForkHeight != forkHeightRange)
                LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? - No more data (Amount)\n",
                          nHeight, nForkHeight, forkHeightRange);
            break;
        }
        //public key
        char pubkeysize[8] = {};
        //cout this guy
        if (!if_utxo.read(pubkeysize, 8)) {
            LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? - Not more data (PubKeyScript size)\n",
                      nHeight, nForkHeight, forkHeightRange);
            break;
        }
        //convert to base 64
        int pbsize = bytes2uint64(pubkeysize);
        if (pbsize == 0) {
            LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? -  PubKeyScript size = 0\n",
                      nHeight, nForkHeight, forkHeightRange);
            //but proceed
        }
        //Initialize array of characters that is the size of pubkey?
        std::unique_ptr<char[]> pubKeyScript(new char[pbsize]);

        if (!if_utxo.read(&pubKeyScript[0], pbsize)) {
            LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? Not more data (PubKeyScript)\n",
                      nHeight, nForkHeight, forkHeightRange);
            break;
        }
////////////////////////////////////////////////////////////////////////

        //Needs ut64 for files? Part of .bin?
        uint64_t amount = bytes2uint64(coin);
        //makes array into string
        unsigned char* pks = (unsigned char*)pubKeyScript.get();

        // Add coinbase tx's
        CMutableTransaction txNew;
        txNew.vin.resize(1);
        //No input cuz coinbase
        txNew.vin[0].prevout.SetNull();
        //Just create
        txNew.vout.resize(1);
        txNew.vout[0].scriptPubKey = CScript(pks, pks+pbsize);

        //coin value
        txNew.vout[0].nValue = amount;
        
        // for adddresses with coins, double their value on ANON network
        if (!txNew.vout[0].nValue == 0) {
            txNew.vout[0].nValue = amount * 2;
        }

        if(nBlockTx == 0)
            txNew.vin[0].scriptSig = CScript() << nHeight << CScriptNum(nBlockTx) << ToByteVector(hashPid) << OP_0;
        else
            txNew.vin[0].scriptSig = CScript() << nHeight << CScriptNum(nBlockTx) << OP_0;

        unsigned int nTxSize = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
        if (nBlockSize + nTxSize >= nBlockMaxSize)
        {
            LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: %u: block would exceed max size\n",
                      nHeight, nForkHeight, forkHeightRange, nBlockTx);
            break;
        }

        // Legacy limits on sigOps:
        unsigned int nTxSigOps = GetLegacySigOpCount(txNew);
        if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
        {
            LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: %u: block would exceed max sigops\n",
                      nHeight, nForkHeight, forkHeightRange, nBlockTx);
            break;
        }

        pblock->vtx.push_back(txNew);
        pblocktemplate->vTxFees.push_back(0);
        pblocktemplate->vTxSigOps.push_back(nTxSigOps);
        nBlockSize += nTxSize;
        nBlockSigOps += nTxSigOps;
        nBlockTotalAmount += amount;
        ++nBlockTx;

        if (!if_utxo.read(&term, 1) || term != '\n') {
            LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: invalid record separator\n",
                      nHeight, nForkHeight, forkHeightRange);
            break;
        }
    }


    }
    LogPrintf("CreateNewForkBlock(): [%u, %u of %u]: txns=%u size=%u amount=%u sigops=%u\n",
              nHeight, nForkHeight, forkHeightRange, nBlockTx, nBlockSize, nBlockTotalAmount, nBlockSigOps);

    // Randomise nonce
    arith_uint256 nonce = UintToArith256(GetRandHash());
    // Clear the top and bottom 16 bits (for local use as thread flags and counters)
    nonce <<= 32;
    nonce >>= 16;
    pblock->nNonce = ArithToUint256(nonce);

    // Fill in header
    pblock->hashReserved = forkExtraHashSentinel;
    pblock->nSolution.clear();
    LogPrintf("End of createforkblock - size of the block: %d \n", pblock->vtx.size());
    return pblocktemplate.release();
}
//////////Loops over all UTXO/Joinsplit data and adds to block as coinbase
// CBlockTemplate* CreateNewForkBlock(bool& bFileNotFound, const int nHeight)

// {


//     // Create new block
//     std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
//     if(!pblocktemplate.get())
//         return NULL;
//     CBlock *pblock = &pblocktemplate->block; // pointer for convenience

//       // Largest block you're willing to create:
//     unsigned int nBlockMaxSize = (unsigned int)(MAX_BLOCK_SIZE-1000);

//     uint64_t nBlockTotalAmount = 0;
//     uint64_t nBlockSize = 1000;
//     uint64_t nBlockTx = 0;
//     uint64_t nBlockSigOps = 100;

//     const CChainParams& chainparams = Params();

//     //const int nForkHeight = nHeight - forkStartHeight;
//     const int nForkHeight = 0;

//     //Here is the UTXO directory, which file we will read from
//     string utxo_file_path = GetUTXOFileName(nHeight);

//     //Read from the specified UTXO file
//     std::ifstream utxo_data(utxo_file_path, std::ios::binary | std::ios::in);

//     LogPrintf("UTXO FILE PATH: %s\n", utxo_file_path);
//     if (!utxo_data.is_open()) {

//         string zutxo_file_path = GetUTXOFileName(nHeight, true);
//         std::ifstream zutxo_data(zutxo_file_path, std::ios::binary | std::ios::in);

//         //UT
//         CMutableTransaction txZ;
//         if(!zutxo_data.is_open()){
//             LogPrintf("ERROR: CreateNewForkBlock(): [%u, of ]: Cannot open ZUTXO file - %s\n",
//                   nHeight, zutxo_file_path);
//             bFileNotFound = true;
//             LogPrintf("ERROR: CreateNewForkBlock(): [%u,  of ]: Cannot open UTXO file - %s\n",
//                     nHeight, utxo_file_path);
//             return NULL;
//         } else {

//             CTransaction txOut;
//             CFlatDB<CTransaction> flatdb2("z-address-test.dat", "zAddressCache");
//                 if(!flatdb2.Load(txOut)) {
//                     return NULL;
//                     LogPrintf("Failed to load transactions from z-address.dat");
//                     }
//                 LogPrintf("-------------------TX OUT----------------------");
//                 LogPrintf("V1 size: %d\n", txOut.vjoinsplit.size());
//                 LogPrintf("TX: %d\n", txOut.ToString());
//                 LogPrintf("Vin: %d\n", txOut.vin[0].ToString());
//                 LogPrintf("Vout: %d\n", txOut.vout[0].ToString());
//                 LogPrintf("V size: %d\n", txOut.vjoinsplit.size());
//                 LogPrintf("TXID: %d\n", txOut.GetHash().ToString());


//             //  CFlatDB<CMutableTransaction> flatdb2("z-address-test.dat", "zAddressCache");
//             // if(!flatdb2.Load(txZ)) {
//             //     LogPrintf("Failed to load transactions from z-address.dat\n");
//             //     return NULL;
//             // }

//                 // // Add coinbase tx'
//             //txZ.vin = NULL;
//             //No input cuz coinbase
//             txZ.vin[0].prevout.SetNull();
//             //Just create
//             txZ.vout.clear();
//             //txZ.vout[0].scriptPubKey = CScript(pks, pks+pbsize);

//             //PUSHING INTO BLOCK
//             pblock->vtx.push_back(txZ);
//             pblocktemplate->vTxFees.push_back(0);
//             //pblocktemplate->vTxSigOps.push_back(nTxSigOps);
//             //nBlockSize += nTxSize;
//             // nBlockSigOps += nTxSigOps;
//             //nBlockTotalAmount += amount;
//             ++nBlockTx;

//             // Randomise nonce
//             arith_uint256 nonce = UintToArith256(GetRandHash());
//             // Clear the top and bottom 16 bits (for local use as thread flags and counters)
//             nonce <<= 32;
//             nonce >>= 16;
//             pblock->nNonce = ArithToUint256(nonce);

//             // Fill in header
//             pblock->hashReserved   = forkExtraHashSentinel;
//             pblock->nSolution.clear();

//             return pblocktemplate.release();
//          }
//             LogPrintf("-------------------------------------------------\n");
//             LogPrintf("-------------------------------------------------\n");
//             // LogPrintf("TX %s\n", txZ.ToString());
//             LogPrintf("TX vjoinsplit size: %d\n", txZ.vjoinsplit.size());
//             LogPrintf("TX joinSplitPubKey size: %d\n", txZ.joinSplitPubKey.GetHex());


//             LogPrintf("TX vjoinsplit size: %d\n", txZ.vjoinsplit[0].vpub_old);


//             //  for(auto &vj : txZ.vjoinsplit) {
//             //     LogPrintf("Vpub_old: %s\n", vj.vpub_old);
//             //     LogPrintf("vpub_new: %s\n", vj.vpub_new);
//             //     LogPrintf("anchor: %s\n", vj.anchor.GetHex());
//             //     LogPrintf("ephemeralKey: %s\n", vj.ephemeralKey.GetHex());
//             //     LogPrintf("randomSeed: %s\n", vj.randomSeed.GetHex());

//             //     for(auto &nf : vj.nullifiers) {
//             //         LogPrintf("nullifier: %s\n", nf.GetHex());
//             //     }
//             //     for(auto &cm : vj.commitments) {
//             //         LogPrintf("commitment: %s\n", cm.GetHex());
//             //     }
//             //     for(auto &ct : vj.ciphertexts) {
//             //         LogPrintf("ciphertext: ");
//             //         for(auto &c : ct) {
//             //             LogPrintf("%d",(int)c);
//             //             }
//             //         LogPrintf("\n");
//             //     }
//             //     for(auto &mac : vj.macs) {
//             //         LogPrintf("mac: %s\n", mac.GetHex());
//             //     }
//             // }
//             LogPrintf("-------------------------------------------------\n");
//             LogPrintf("-------------------------------------------------\n");
//             // LogPrintf("-----------LOADING JSON--------------------------\n");
//         // std::ifstream i("test.json");
//         // LogPrintf("--after loading file\n");
//         // nlohmann:: json j;
//         // i >> j;
//         //LogPrintf("--after parsing file\n");
//         //LogPrintf("JSON -> vjoinsplit size: %d\n", j.at("blocks")[3].at("transactions")[0].at("vpub_old"));
//         //LogPrintf("JSON -> vjoinsplit size: \n%s\n", j.dump(4));


//         //JSON EXAMPLE

// //         int main()
// //  {
// //      // create JSON object
// //      json object =
// //      {
// //          {"the good", "il buono"},
// //          {"the bad", "il cativo"},
// //          {"the ugly", "il brutto"}
// //      };

// //      // output element with key "the ugly"
// //      std::cout << object.at("the ugly") << '\n';

// //      // change element with key "the bad"
// //      object.at("the bad") = "il cattivo";

// //      // output changed array
// //      std::cout << object << '\n';

// //      // try to write at a nonexisting key
// //      try
// //      {
// //          object.at("the fast") = "il rapido";
// //      }
// //      catch (std::out_of_range& e)
// //      {
// //          std::cout << "out of range: " << e.what() << '\n';
// //      }
// //  }

//         // // Add coinbase tx's
//         // CMutableTransaction txNew;
//         // txNew.vin.resize(1);
//         // //No input cuz coinbase
//         // txNew.vin[0].prevout.SetNull();
//         // //Just create
//         // txNew.vout.resize(1);
//         // txNew.vout[0].scriptPubKey = CScript(pks, pks+pbsize);

//         // //coin value
//         // txNew.vout[0].nValue = amount;

//         //     //PUSHING INTO BLOCK
//         //     pblock->vtx.push_back(txNew);
//         //     pblocktemplate->vTxFees.push_back(0);
//         //     pblocktemplate->vTxSigOps.push_back(nTxSigOps);
//         //     nBlockSize += nTxSize;
//         //     nBlockSigOps += nTxSigOps;
//         //     nBlockTotalAmount += amount;
//         //     ++nBlockTx;


//             LogPrintf("Do THE THINGS!-------------------------------------%s\n", zutxo_file_path);
//         }

// /////////////////////////////////zk-stuff////////////////////////////////////////
//     //If the file name has "zk" follow zk rules, else go to original rules

//     LogPrintf("ZKSHOULD BE STARTING HERE>>>>>>>>>>>>");
//     if (utxo_file_path.find('z') != -1){
//     //   /////TODO Test 1 Test check to see if UTXO or ZK txs file is recognized
//         std::cout << "We're In zk!" << std::endl;
//         LogPrintf("ZK found >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
//         LogPrintf("BTCPrivate Miner: switching into fork mode\n");
//     } else {
//         std::cout << "zk not found" << std::endl;
//         LogPrintf("zk not found >>>>>>>>>>>>>>>>>>>>>");
//     }

//     ////////////////////////END ZK Stuff///////////////////////////

//     //UTXO READ
//     while (utxo_data && nBlockTx < forkCBPerBlock) {
//         char term = 0;
// /////////Format checks, explore more when looking at UTXO raw
//         //Value
//         char coin[8] = {};
//         if (!utxo_data.read(coin, 8)) {
//             // the last file may be shorter than forkCBPerBlock
//             if(!utxo_data.eof() || nForkHeight != forkHeightRange)
//                 LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? - No more data (Amount)\n",
//                           nHeight, nForkHeight, forkHeightRange);
//             break;
//         }
//         //public key
//         char pubkeysize[8] = {};
//         //cout this guy
//         if (!utxo_data.read(pubkeysize, 8)) {
//             LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? - Not more data (PubKeyScript size)\n",
//                       nHeight, nForkHeight, forkHeightRange);
//             break;
//         }
//         //convert to base 64
//         int pbsize = bytes2uint64(pubkeysize);
//         if (pbsize == 0) {
//             LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? -  PubKeyScript size = 0\n",
//                       nHeight, nForkHeight, forkHeightRange);
//             //but proceed
//         }
//         //Initialize array of characters that is the size of pubkey?
//         std::unique_ptr<char[]> pubKeyScript(new char[pbsize]);

//         if (!utxo_data.read(&pubKeyScript[0], pbsize)) {
//             LogPrintf("ERROR: CreateNewForkBlock(): [%u, %u of %u]: UTXO file corrupted? Not more data (PubKeyScript)\n",
//                       nHeight, nForkHeight, forkHeightRange);
//             break;
//         }
// //////////////READ END//////////////////////////////////////////////////////////

//         //UTXO FORMATING
//         //Needs ut64 for files? Part of .bin?
//         uint64_t amount = bytes2uint64(coin);
//         //makes array into string
//         unsigned char* pks = (unsigned char*)pubKeyScript.get();

//         // // Add coinbase tx's
//         CMutableTransaction txNew;
//         txNew.vin.resize(1);
//         //No input cuz coinbase
//         txNew.vin[0].prevout.SetNull();
//         //Just create
//         txNew.vout.resize(1);
//         txNew.vout[0].scriptPubKey = CScript(pks, pks+pbsize);

//         //coin value
//         txNew.vout[0].nValue = amount;
//         if(nBlockTx == 0)
//             txNew.vin[0].scriptSig = CScript() << nHeight << CScriptNum(nBlockTx) << ToByteVector(hashPid) << OP_0;
//         else
//             txNew.vin[0].scriptSig = CScript() << nHeight << CScriptNum(nBlockTx) << OP_0;

//         unsigned int nTxSize = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
//         if (nBlockSize + nTxSize >= nBlockMaxSize)
//         {
//             LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: %u: block would exceed max size\n",
//                       nHeight, nForkHeight, forkHeightRange, nBlockTx);
//             break;
//         }

//         // Legacy limits on sigOps:
//         unsigned int nTxSigOps = GetLegacySigOpCount(txNew);
//         if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
//         {
//             LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: %u: block would exceed max sigops\n",
//                       nHeight, nForkHeight, forkHeightRange, nBlockTx);
//             break;
//         }

//         //PUSHING INTO BLOCK
//         pblock->vtx.push_back(txNew);
//         pblocktemplate->vTxFees.push_back(0);
//         pblocktemplate->vTxSigOps.push_back(nTxSigOps);
//         nBlockSize += nTxSize;
//         nBlockSigOps += nTxSigOps;
//         nBlockTotalAmount += amount;
//         ++nBlockTx;

//         if (!utxo_data.read(&term, 1) || term != '\n') {
//             LogPrintf("ERROR:  CreateNewForkBlock(): [%u, %u of %u]: invalid record separator\n",
//                       nHeight, nForkHeight, forkHeightRange);
//             break;
//         }
//     }
//     LogPrintf("CreateNewForkBlock(): [%u, %u of %u]: txns=%u size=%u amount=%u sigops=%u\n",
//               nHeight, nForkHeight, forkHeightRange, nBlockTx, nBlockSize, nBlockTotalAmount, nBlockSigOps);

//     // Randomise nonce
//     arith_uint256 nonce = UintToArith256(GetRandHash());
//     // Clear the top and bottom 16 bits (for local use as thread flags and counters)
//     nonce <<= 32;
//     nonce >>= 16;
//     pblock->nNonce = ArithToUint256(nonce);

//     // Fill in header
//     pblock->hashReserved   = forkExtraHashSentinel;
//     pblock->nSolution.clear();

//     return pblocktemplate.release();
// }

CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn)
{
    const CChainParams& chainparams = Params();
    // Create new block
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if (!pblocktemplate.get())
        return NULL;
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1);   // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE - 1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CAmount nFees = 0;

    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;
        pblock->nTime = GetAdjustedTime();
        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();
        pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
        // -regtest only: allow overriding block.nVersion with
        // -blockversion=N to test forking scenarios
        if (chainparams.MineBlocksOnDemand())
            pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

        CCoinsViewCache view(pcoinsTip);

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*>> mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", false);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTxMemPoolEntry>::iterator mi = mempool.mapTx.begin();
             mi != mempool.mapTx.end(); ++mi) {
            const CTransaction& tx = mi->second.GetTx();

            int64_t nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST) ? nMedianTimePast : pblock->GetBlockTime();

            if (tx.IsCoinBase() || !IsFinalTx(tx, nHeight, nLockTimeCutoff))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            CAmount nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH (const CTxIn& txin, tx.vin) {
                // Read prev transaction
                if (!view.HaveCoins(txin.prevout.hash)) {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash)) {
                        LogPrintf("ERROR: mempool transaction missing input\n");
                        if (fDebug)
                            assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan) {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].GetTx().vout[txin.prevout.n].nValue;
                    continue;
                }
                const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                assert(coins);

                CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = nHeight - coins->nHeight;

                dPriority += (double)nValueIn * nConf;
            }
            nTotalIn += tx.GetJoinSplitValueIn();

            if (fMissingInputs)
                continue;

            // Priority is sum(valuein * age) / modified_txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            uint256 hash = tx.GetHash();
            mempool.ApplyDeltas(hash, dPriority, nTotalIn);

            CFeeRate feeRate(nTotalIn - tx.GetValueOut(), nTxSize);

            if (porphan) {
                porphan->dPriority = dPriority;
                porphan->feeRate = feeRate;
            } else
                vecPriority.push_back(TxPriority(dPriority, feeRate, &mi->second.GetTx()));
        }

        // Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty()) {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Skip free transactions if we're past the minimum block size:
            const uint256& hash = tx.GetHash();
            double dPriorityDelta = 0;
            CAmount nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
            if (fSortedByFee && (dPriorityDelta <= 0) && (nFeeDelta <= 0) && (feeRate < ::minRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritise by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority))) {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx))
                continue;

            CAmount nTxFees = view.GetValueIn(tx) - tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            CValidationState state;
            if (!ContextualCheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, Params().GetConsensus()))
                continue;

            UpdateCoins(tx, state, view, nHeight);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority) {
                LogPrintf("priority %.1f fee %s txid %s\n",
                          dPriority, feeRate.ToString(), tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash)) {
                BOOST_FOREACH (COrphan* porphan, mapDependers[hash]) {
                    if (!porphan->setDependsOn.empty()) {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty()) {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("CreateNewBlock(): total size %u\n", nBlockSize);

        // Create coinbase tx
        CMutableTransaction txNew;
        txNew.vin.resize(1);
        txNew.vin[0].prevout.SetNull();
        txNew.vout.resize(1);
        txNew.vout[0].scriptPubKey = scriptPubKeyIn;
        txNew.vout[0].nValue = GetBlockSubsidy(nHeight, chainparams.GetConsensus());

        // Add fees
        txNew.vout[0].nValue += nFees;
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0;

        pblock->vtx[0] = txNew;
        pblocktemplate->vTxFees[0] = -nFees;

        // Randomise nonce
        arith_uint256 nonce = UintToArith256(GetRandHash());
        // Clear the top and bottom 16 bits (for local use as thread flags and counters)
        nonce <<= 32;
        nonce >>= 16;
        pblock->nNonce = ArithToUint256(nonce);

        // Fill in header
        pblock->hashPrevBlock = pindexPrev->GetBlockHash();
        pblock->hashReserved = uint256();
        UpdateTime(pblock, Params().GetConsensus(), pindexPrev);
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
        pblock->nSolution.clear();
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        CValidationState state;
        if (!TestBlockValidity(state, *pblock, pindexPrev, false, false))
            throw std::runtime_error("CreateNewBlock(): TestBlockValidity failed");
    }

    return pblocktemplate.release();
}

#ifdef ENABLE_WALLET
boost::optional<CScript> GetMinerScriptPubKey(CReserveKey& reservekey)
#else
boost::optional<CScript> GetMinerScriptPubKey()
#endif
{
    CKeyID keyID;
    CBitcoinAddress addr;
    if (addr.SetString(GetArg("-mineraddress", ""))) {
        addr.GetKeyID(keyID);
    } else {
#ifdef ENABLE_WALLET
        CPubKey pubkey;
        if (!reservekey.GetReservedKey(pubkey)) {
            return boost::optional<CScript>();
        }
        keyID = pubkey.GetID();
#else
        return boost::optional<CScript>();
#endif
    }

    CScript scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(keyID) << OP_EQUALVERIFY << OP_CHECKSIG;
    return scriptPubKey;
}

#ifdef ENABLE_WALLET
CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey)
{
    boost::optional<CScript> scriptPubKey = GetMinerScriptPubKey(reservekey);
#else
CBlockTemplate* CreateNewBlockWithKey()
{
    boost::optional<CScript> scriptPubKey = GetMinerScriptPubKey();
#endif

    if (!scriptPubKey) {
        return NULL;
    }
    return CreateNewBlock(*scriptPubKey);
}

/////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

#ifdef ENABLE_MINING

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}

#ifdef ENABLE_WALLET
static bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
#else
static bool ProcessBlockFound(CBlock* pblock)
#endif // ENABLE_WALLET
{
    if (!looksLikeForkBlockHeader(*pblock)) {
        LogPrintf("%s\n", pblock->ToString());
    }

    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("BTCPrivate Miner: generated block is stale");
    }

#ifdef ENABLE_WALLET
    if (GetArg("-mineraddress", "").empty()) {
        // Remove key from key pool
        reservekey.KeepKey();
    }

    // Track how many getdata requests this block gets
    {
        LOCK(wallet.cs_wallet);
        wallet.mapRequestCount[pblock->GetHash()] = 0;
    }
#endif

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, NULL, pblock, true, NULL))
        return error("BTCPrivate Miner: ProcessNewBlock, block not accepted");

    TrackMinedBlock(pblock->GetHash());

    return true;
}

#ifdef ENABLE_WALLET
void static BitcoinMiner(CWallet* pwallet)
#else
void static BitcoinMiner()
#endif
{
    LogPrintf("BTCPrivate Miner started %s\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("btcp-miner");
    const CChainParams& chainparams = Params();

#ifdef ENABLE_WALLET
    // Each thread has its own key
    CReserveKey reservekey(pwallet);
#endif

    // Each thread has its own counter
    unsigned int nExtraNonce = 0;

    unsigned int n = chainparams.EquihashN();
    unsigned int k = chainparams.EquihashK();

    std::string solver = GetArg("-equihashsolver", "default");
    assert(solver == "tromp" || solver == "default");
    LogPrint("pow", "Using Equihash solver \"%s\" with n = %u, k = %u\n", solver, n, k);

    std::mutex m_cs;
    bool cancelSolver = false;
    boost::signals2::connection c = uiInterface.NotifyBlockTip.connect(
        [&m_cs, &cancelSolver](const uint256& hashNewTip) mutable {
            std::lock_guard<std::mutex> lock{m_cs};
            cancelSolver = true;
        });
    miningTimer.start();

    bool bForkModeStarted = false;
    try {
        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                bool fForkMiner = GetBoolArg("-fork-mine", false);

                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                miningTimer.stop();
                do {
                    bool fvNodesEmpty;
                    {
                        LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }
                    if (!fvNodesEmpty && (fForkMiner || !IsInitialBlockDownload(true)))
                        break;
                    MilliSleep(1000);
                } while (true);
                miningTimer.start();
            }

            CBlockIndex* pindexPrev = chainActive.Tip();
            CBlock* pblock = nullptr;
            unsigned int nTransactionsUpdatedLast = 0;

            //
            // Create new block
            //
            unique_ptr<CBlockTemplate> pblocktemplate;

            bool isNextBlockFork = isForkBlock(pindexPrev->nHeight + 1);

            if (isNextBlockFork) {
                if (!bForkModeStarted) {
                    LogPrintf("BTCPrivate Miner: switching into fork mode\n");
                    bForkModeStarted = true;
                }

                bool bFileNotFound = false;
                LogPrintf("BEFORE\n");
                pblocktemplate.reset(CreateNewForkBlock(bFileNotFound));
                LogPrintf("AFTER\n");
                if (!pblocktemplate.get()) {
                    if (bFileNotFound) {
                        MilliSleep(1000);
                        continue;
                    } else {
                        LogPrintf("Error in BTCPrivate Miner: Cannot create Fork Block\n");
                        return;
                    }
                }
                pblock = &pblocktemplate->block;
                pblock->hashMerkleRoot = pblock->BuildMerkleTree();

                LogPrintf("Running BTCPrivate Miner with %u forking transactions in block (%u bytes) and N = %d, K = %d\n",
                          pblock->vtx.size(),
                          ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION),
                          n, k);
            } else {
                //if not in forking mode and/or provided file is read to the end - exit
                if (bForkModeStarted) {
                    LogPrintf("BTCPrivate Miner: Fork is done - switching back to regular miner\n");
                    bForkModeStarted = false;
                }

                nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();

#ifdef ENABLE_WALLET
                pblocktemplate.reset(CreateNewBlockWithKey(reservekey));
#else
                pblocktemplate.reset(CreateNewBlockWithKey());
#endif
                if (!pblocktemplate.get()) {
                    if (GetArg("-mineraddress", "").empty()) {
                        LogPrintf("Error in BTCPrivate Miner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                    } else {
                        // Should never reach here, because -mineraddress validity is checked in init.cpp
                        LogPrintf("Error in BTCPrivate Miner: Invalid -mineraddress\n");
                    }
                    return;
                }
                pblock = &pblocktemplate->block;

                LogPrintf("Running BTCPrivate Miner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                          ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

                IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
            } //else


            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

            while (true) {
                // Hash state
                crypto_generichash_blake2b_state state;
                EhInitialiseState(n, k, state);

                // I = the block header minus nonce and solution.
                CEquihashInput I{*pblock};
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << I;

                // H(I||...
                crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());

                // H(I||V||...
                crypto_generichash_blake2b_state curr_state;
                curr_state = state;
                crypto_generichash_blake2b_update(&curr_state,
                                                  pblock->nNonce.begin(),
                                                  pblock->nNonce.size());

                // (x_1, x_2, ...) = A(I, V, n, k)
                LogPrint("pow", "Running Equihash solver \"%s\" with nNonce = %s\n",
                         solver, pblock->nNonce.ToString());

                std::function<bool(std::vector<unsigned char>)> validBlock =
                    [&pblock, &hashTarget, &m_cs, &cancelSolver, &chainparams
#ifdef ENABLE_WALLET
                     ,
                     &pwallet, &reservekey
#endif
                     ,
                     &isNextBlockFork](std::vector<unsigned char> soln) {
                        // Write the solution to the hash and compute the result.
                        LogPrint("pow", "- Checking solution against target\n");
                        pblock->nSolution = soln;
                        solutionTargetChecks.increment();

                        if (UintToArith256(pblock->GetHash()) > hashTarget) {
                            return false;
                        }

                        // Found a solution
                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("BTCPrivate Miner:\n");
                        LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", pblock->GetHash().GetHex(), hashTarget.GetHex());

                        if (ProcessBlockFound(pblock
#ifdef ENABLE_WALLET
                                              ,
                                              *pwallet, reservekey
#endif
                                              )) {
                            // Ignore chain updates caused by us
                            std::lock_guard<std::mutex> lock{m_cs};
                            cancelSolver = false;
                        }
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);

                        // In regression test mode, stop mining after a block is found.
                        if (chainparams.MineBlocksOnDemand() && !isNextBlockFork) {
                            // Increment here because throwing skips the call below
                            ehSolverRuns.increment();
                            throw boost::thread_interrupted();
                        }

                        return true;
                    };

                std::function<bool(EhSolverCancelCheck)> cancelled = [&m_cs, &cancelSolver](EhSolverCancelCheck pos) {
                    std::lock_guard<std::mutex> lock{m_cs};
                    return cancelSolver;
                };

                // TODO: factor this out into a function with the same API for each solver.
                if (solver == "tromp") {
                    // Create solver and initialize it.
                    equi eq(1);
                    eq.setstate(&curr_state);

                    // Intialization done, start algo driver.
                    eq.digit0(0);
                    eq.xfull = eq.bfull = eq.hfull = 0;
                    eq.showbsizes(0);
                    for (u32 r = 1; r < WK; r++) {
                        (r & 1) ? eq.digitodd(r, 0) : eq.digiteven(r, 0);
                        eq.xfull = eq.bfull = eq.hfull = 0;
                        eq.showbsizes(r);
                    }
                    eq.digitK(0);
                    ehSolverRuns.increment();

                    // Convert solution indices to byte array (decompress) and pass it to validBlock method.
                    for (size_t s = 0; s < eq.nsols; s++) {
                        LogPrint("pow", "Checking solution %d\n", s + 1);
                        std::vector<eh_index> index_vector(PROOFSIZE);
                        for (size_t i = 0; i < PROOFSIZE; i++) {
                            index_vector[i] = eq.sols[s][i];
                        }
                        std::vector<unsigned char> sol_char = GetMinimalFromIndices(index_vector, DIGITBITS);

                        if (validBlock(sol_char)) {
                            // If we find a POW solution, do not try other solutions
                            // because they become invalid as we created a new block in blockchain.
                            break;
                        }
                    }
                } else {
                    try {
                        // If we find a valid block, we rebuild
                        bool found = EhOptimisedSolve(n, k, curr_state, validBlock, cancelled);
                        ehSolverRuns.increment();
                        if (found) {
                            break;
                        }
                    } catch (EhSolverCancelledException&) {
                        LogPrint("pow", "Equihash solver cancelled\n");
                        std::lock_guard<std::mutex> lock{m_cs};
                        cancelSolver = false;
                    }
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();

                // Regtest mode doesn't require peers
                if (vNodes.empty() && chainparams.MiningRequiresPeers())
                    break;
                if ((UintToArith256(pblock->nNonce) & 0xffff) == 0xffff)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nNonce and nTime
                pblock->nNonce = ArithToUint256(UintToArith256(pblock->nNonce) + 1);
                UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);

                if (chainparams.GetConsensus().fPowAllowMinDifficultyBlocks) {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    } catch (const boost::thread_interrupted&) {
        miningTimer.stop();
        c.disconnect();
        LogPrintf("BTCPrivate Miner terminated\n");
        throw;
    } catch (const std::runtime_error& e) {
        miningTimer.stop();
        c.disconnect();
        LogPrintf("BTCPrivate Miner runtime error: %s\n", e.what());
        return;
    }
    miningTimer.stop();
    c.disconnect();
}

#ifdef ENABLE_WALLET
void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads)
#else
void GenerateBitcoins(bool fGenerate, int nThreads)
#endif
{
    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0)
        nThreads = GetNumCores();

    if (minerThreads != NULL) {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++) {
#ifdef ENABLE_WALLET
        minerThreads->create_thread(boost::bind(&BitcoinMiner, pwallet));
#else
        minerThreads->create_thread(&BitcoinMiner);
#endif
    }
}

#endif // ENABLE_MINING


UniValue decoderawtransaction2(CTransaction &tx , const UniValue& params, bool fHelp)
{
    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));
    // CTransaction tx;
    if (!DecodeHexTx(tx, params.get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    UniValue result(UniValue::VOBJ);
    TxToJSON(tx, uint256(), result);

    string strJSON = result.write() + "\n";
    LogPrintf("JSON: \n");
    LogPrintf("%s", strJSON);

    return result;
}