//
//  block_processor.cpp
//  BlockParser2
//
//  Created by Harry Kalodner on 1/11/17.
//  Copyright © 2017 Harry Kalodner. All rights reserved.
//

#define BLOCKSCI_WITHOUT_SINGLETON

#include "block_processor.hpp"
#include "script_input.hpp"
#include "file_writer.hpp"
#include "safe_mem_reader.hpp"
#include "chain_index.hpp"
#include "preproccessed_block.hpp"
#include "chain_index.hpp"
#include "utxo_state.hpp"
#include "address_state.hpp"

#include <blocksci/hash.hpp>
#include <blocksci/bitcoin_uint256.hpp>
#include <blocksci/chain/block.hpp>
#include <blocksci/chain/input.hpp>
#include <blocksci/chain/output.hpp>
#include <blocksci/chain/transaction.hpp>
#include <blocksci/scripts/bitcoin_pubkey.hpp>

#ifdef BLOCKSCI_RPC_PARSER
#include <bitcoinapi/bitcoinapi.h>
#endif

#include <thread>
#include <fstream>
#include <iostream>

std::vector<unsigned char> ParseHex(const char* psz);

BlockProcessor::BlockProcessor(uint32_t startingTxCount_, uint32_t totalTxCount_, uint32_t maxBlockHeight_) : startingTxCount(startingTxCount_), currentTxNum(startingTxCount_), totalTxCount(totalTxCount_), maxBlockHeight(maxBlockHeight_) {
    
}

std::vector<unsigned char> ParseHex(const char* psz)
{
    // convert hex dump to vector
    std::vector<unsigned char> vch;
    while (true)
    {
        while (isspace(*psz))
            psz++;
        signed char c = blocksci::HexDigit(*psz++);
        if (c == static_cast<signed char>(-1))
            break;
        unsigned char n = static_cast<unsigned char>(c << 4);
        c = blocksci::HexDigit(*psz++);
        if (c == static_cast<signed char>(-1))
            break;
        n |= c;
        vch.push_back(n);
    }
    return vch;
}

blocksci::Block getBlock(uint32_t firstTxIndex, uint32_t txCount, size_t coinbasePos, const BlockInfoBase &block);

blocksci::Block getBlock(uint32_t firstTxIndex, uint32_t txCount, size_t coinbasePos, const BlockInfoBase &block) {
    return {firstTxIndex, txCount, static_cast<uint32_t>(block.height), block.hash, block.header.nVersion, block.header.nTime, block.header.nBits, block.header.nNonce, coinbasePos};
}

#ifdef BLOCKSCI_FILE_PARSER

void BlockProcessor::closeFinishedFiles(uint32_t txNum) {
    auto it = files.begin();
    while (it != files.end()) {
        if (it->second.second < txNum) {
            it = files.erase(it);
        } else {
            ++it;
        }
    }
}

struct SegwitChecker : public boost::static_visitor<bool> {
    bool operator()(const ScriptOutput<blocksci::AddressType::Enum::NULL_DATA> &output) const {
        uint32_t segwitMarker = *reinterpret_cast<const uint32_t *>(output.fullData.data());
        return segwitMarker == 0xeda921aa;
    }
    
    template <blocksci::AddressType::Enum type>
    bool operator()(const ScriptOutput<type> &) const {
        return false;
    }
};

bool checkSegwit(RawTransaction *tx, const SegwitChecker &checker);

bool checkSegwit(RawTransaction *tx, const SegwitChecker &checker) {
    for (int i = static_cast<int>(tx->outputs.size()) - 1; i >= 0; i--) {
        if (boost::apply_visitor(checker, tx->outputs[static_cast<size_t>(i)].scriptOutput)) {
            return true;
        }
    }
    return false;
}

void BlockProcessor::readNewBlocks(const ParserConfiguration<FileTag> &config, std::vector<BlockInfo<FileTag>> blocksToAdd) {
    using namespace std::chrono_literals;
    
    std::unordered_map<int, uint32_t> firstTimeRequired;
    std::unordered_map<int, uint32_t> lastBlockRequired;
    std::unordered_map<int, uint32_t> lastTxRequired;
    
    auto firstTxNum = currentTxNum;
    
    int maxBlockFile = 0;
    int minBlockFile = std::numeric_limits<int>::max();
    for (auto &block : blocksToAdd) {
        firstTxNum += block.nTx;
        firstTimeRequired.insert(std::make_pair(block.nFile, block.height));
        lastBlockRequired[block.nFile] = static_cast<uint32_t>(block.height);
        lastTxRequired[block.nFile] = firstTxNum;
        minBlockFile = std::min(block.nFile, minBlockFile);
        maxBlockFile = std::max(block.nFile, maxBlockFile);
    }
    
    blocksci::ArbitraryFileMapper<boost::iostreams::mapped_file::readwrite> blockCoinbaseFile(config.blockCoinbaseFilePath());
    blocksci::FixedSizeFileMapper<blocksci::Block, boost::iostreams::mapped_file::readwrite> blockFile(config.blockFilePath());
    blocksci::IndexedFileMapper<boost::iostreams::mapped_file::readwrite, uint32_t> sequenceFile{config.sequenceFilePath()};
    
    blocksci::uint256 nullHash;
    nullHash.SetNull();
    
    SegwitChecker checker;
    
    std::vector<unsigned char> coinbase;
    
    for (uint32_t i = 0; i < blocksToAdd.size(); i++) {
        auto &block = blocksToAdd[i];
        
        auto fileIt = files.find(block.nFile);
        if (fileIt == files.end()) {
            auto blockPath = config.pathForBlockFile(block.nFile);
            if (!boost::filesystem::exists(blockPath)) {
                std::cout << "Error: Failed to open block file " << blockPath << "\n";
                break;
            }
            files.emplace(std::piecewise_construct, std::forward_as_tuple(block.nFile), std::forward_as_tuple(blockPath, lastTxRequired[block.nFile]));
        }
        
        auto &reader = files.at(block.nFile).first;
        
        try {
            reader.reset(block.nDataPos);
            reader.advance(sizeof(CBlockHeader));
            uint32_t txCount = reader.readVariableLengthInteger();
            
            auto firstTxIndex = currentTxNum;
            
            RawTransaction *tx;
            if (!finished_transaction_queue.pop(tx)) {
                tx = new RawTransaction();
            } else {
                closeFinishedFiles(tx->txNum);
            }
            
            bool segwit = false;
            auto firstTxOffset = reader.offset();
            for (uint32_t j = 0; j < txCount; j++) {
                tx->load(reader, 0, 0, false);
                if (tx->inputs.size() == 1 && tx->inputs[0].rawOutputPointer.hash == nullHash) {
                    segwit = checkSegwit(tx, checker);
                    break;
                }
            }
            
            reader.reset(firstTxOffset);
            
            for (uint32_t j = 0; j < txCount; j++) {
                tx->load(reader, currentTxNum, static_cast<uint32_t>(block.height), segwit);
                
                sequenceFile.writeIndexGroup();
                for (auto &input : tx->inputs) {
                    sequenceFile.write(input.sequenceNum);
                }
                
                if (tx->inputs.size() == 1 && tx->inputs[0].rawOutputPointer.hash == nullHash) {
                    auto scriptBegin = tx->inputs[0].getScriptBegin();
                    coinbase.assign(scriptBegin, scriptBegin + tx->inputs[0].getScriptLength());
                    tx->inputs.clear();
                }
                
                while (!hash_transaction_queue.push(tx)) {
                    std::this_thread::sleep_for(100ms);
                }
                currentTxNum++;
                
                if (!finished_transaction_queue.pop(tx)) {
                    tx = new RawTransaction();
                } else {
                    closeFinishedFiles(tx->txNum);
                }
            }
            
            blockFile.write(getBlock(firstTxIndex, txCount, blockCoinbaseFile.size(), block));
            blockCoinbaseFile.write(coinbase.begin(), coinbase.end());
        } catch (const std::exception &e) {
            std::cerr << "Failed to load tx"
            << " from block" << block.height
            << " at offset " << reader.offset()
            << ".\n";
            throw;
        }
    }
    rawDone = true;
}

#endif

#ifdef BLOCKSCI_RPC_PARSER

void BlockProcessor::loadTxRPC(RawTransaction *tx, uint32_t txNum, const BlockInfo<RPCTag> &block, uint32_t txOffset, BitcoinAPI & bapi, bool witnessActivated) {
    if (block.height == 0) {
        tx->outputs.clear();
        tx->outputs.reserve(1);
        
        auto scriptPubKey = CScript() << ParseHex("040184a11fa689ad5123690c81a3a49c8f13f8d45bac857fbcbc8bc4a8ead3eb4b1ff4d4614fa18dce611aaf1f471216fe1b51851b4acf21b17fc45171ac7b13af") << OP_CHECKSIG;
        std::vector<unsigned char> scriptBytes(scriptPubKey.begin(), scriptPubKey.end());
        //Set the desired initial block reward
        tx->outputs.emplace_back(scriptBytes, 50 * 100000000.0, false);
        tx->hash = blocksci::uint256S("0100000000000000000000000000000000000000000000000000000000000000");
        tx->blockHeight = 0;
        tx->txNum = 0;
    } else {
        auto txinfo = bapi.getrawtransaction(block.tx[txOffset], 1);
        tx->load(txinfo, txNum, static_cast<uint32_t>(block.height), witnessActivated);
    }
}

void BlockProcessor::readNewBlocks(const ParserConfiguration<RPCTag> &config, std::vector<BlockInfo<RPCTag>> blocksToAdd) {
    using namespace std::chrono_literals;
    
    blocksci::ArbitraryFileMapper<boost::iostreams::mapped_file::readwrite> blockCoinbaseFile(config.blockCoinbaseFilePath());
    blocksci::FixedSizeFileMapper<blocksci::Block, boost::iostreams::mapped_file::readwrite> blockFile(config.blockFilePath());
    blocksci::IndexedFileMapper<boost::iostreams::mapped_file::readwrite, uint32_t> sequenceFile{config.sequenceFilePath()};
    
    BitcoinAPI bapi{config.createBitcoinAPI()};
    
    blocksci::uint256 nullHash;
    nullHash.SetNull();
    
    RawTransaction *tx;
    SegwitChecker checker;
    std::vector<unsigned char> coinbase;
    
    for (auto &block : blocksToAdd) {
        uint32_t blockTxCount = static_cast<uint32_t>(block.tx.size());
        if (!finished_transaction_queue.pop(tx)) {
            tx = new RawTransaction();
        }
        
        loadTxRPC(tx, 0, block, 0, bapi, false);
        bool segwit = checkSegwit(tx, checker);
        
        auto firstTxIndex = currentTxNum;
        for (uint32_t i = 0; i < blockTxCount; i++) {
            
            if (!finished_transaction_queue.pop(tx)) {
                tx = new RawTransaction();
            }
            loadTxRPC(tx, currentTxNum, block, i, bapi, segwit);
            
            if (tx->inputs.size() == 1 && tx->inputs[0].rawOutputPointer.hash == nullHash) {
                auto scriptBegin = tx->inputs[0].getScriptBegin();
                coinbase.assign(scriptBegin, scriptBegin + tx->inputs[0].getScriptLength());
                tx->inputs.clear();
            }
            
            sequenceFile.writeIndexGroup();
            for (auto &input : tx->inputs) {
                sequenceFile.write(input.sequenceNum);
            }
            
            // Note to self: Currently this does not get the coinbase tx data
            while (!hash_transaction_queue.push(tx)) {
                std::this_thread::sleep_for(100ms);
            }
            currentTxNum++;
        }
        
        blockFile.write(getBlock(firstTxIndex, blockTxCount, blockCoinbaseFile.size(), block));
        blockCoinbaseFile.write(coinbase.begin(), coinbase.end());
    }
    
    rawDone = true;
}

#endif

void BlockProcessor::calculateHashes(const ParserConfigurationBase &config) {
    using namespace std::chrono_literals;
    blocksci::FixedSizeFileMapper<blocksci::uint256, boost::iostreams::mapped_file::readwrite> hashFile{config.txHashesFilePath()};
    auto consume = [&](RawTransaction *tx) -> void {
        tx->calculateHash();
        hashFile.write(tx->hash);
        while (!utxo_transaction_queue.push(tx)) {
            std::this_thread::sleep_for(100ms);
        }
    };
    
    while (!rawDone) {
        while (hash_transaction_queue.consume_all(consume)) {}
        std::this_thread::sleep_for(100ms);
    }
    while (hash_transaction_queue.consume_all(consume)) {}
    
    hashDone = true;
}

void BlockProcessor::processUTXOs(const ParserConfigurationBase &config, UTXOState &utxoState) {
    using namespace std::chrono_literals;
    
    IndexedFileWriter<1> txFile{config.txFilePath()};
    
    auto consume = [&](RawTransaction *tx) -> void {
        
        txFile.writeIndexGroup();
        
        auto diskTx = tx->getRawTransaction();
        assert(diskTx.inputCount == tx->inputs.size());
        assert(diskTx.outputCount == tx->outputs.size());
        
        txFile.write(diskTx);
        
        
        uint16_t i = 0;
        
        for (auto &input : tx->inputs) {
            auto utxo = utxoState.spendOutput(input.rawOutputPointer);
            input.addressType = utxo.addressType;
            input.linkedTxNum = utxo.output.linkedTxNum;
            i++;
            
            blocksci::Address address{0, utxo.addressType};
            blocksci::Inout blocksciInput{utxo.output.linkedTxNum, address, utxo.output.getValue()};
            txFile.write(blocksciInput);
        }
        
        i = 0;
        for (auto &output : tx->outputs) {
            auto type = addressType(output.scriptOutput);
            blocksci::Address address{0, type};
            
            blocksci::Inout blocksciOutput{0, address, output.value};
            txFile.write(blocksciOutput);
            
            if (isSpendable(scriptType(type))) {
                blocksciOutput.linkedTxNum = tx->txNum;
                UTXO utxo{blocksciOutput, type};
                RawOutputPointer pointer{tx->hash, i};
                utxoState.addOutput(utxo, pointer);
            }
            i++;
        }
        
        
        bool flushed = false;
        while (!address_transaction_queue.push(tx)) {
            if (!flushed) {
                txFile.flush();
                flushed = true;
            }
            std::this_thread::sleep_for(100ms);
        }
        
        utxoState.optionalSave();
    };
    
    while (!hashDone) {
        while (utxo_transaction_queue.consume_all(consume)) {}
        std::this_thread::sleep_for(100ms);
    }
    while (utxo_transaction_queue.consume_all(consume)) {}
    
    utxoDone = true;
}

std::vector<uint32_t> BlockProcessor::processAddresses(const ParserConfigurationBase &config, AddressState &addressState) {
    using namespace std::chrono_literals;
    
    using TxFile = blocksci::IndexedFileMapper<boost::iostreams::mapped_file::readwrite, blocksci::RawTransaction>;
    
    auto percentage = static_cast<double>(totalTxCount) / 1000.0;
    
    uint32_t percentageMarker = static_cast<uint32_t>(std::ceil(percentage));
    
    AddressWriter addressWriter{config};
    
    std::cout.setf(std::ios::fixed,std::ios::floatfield);
    std::cout.precision(1);
    
    ECCVerifyHandle handle;
    
    std::vector<uint32_t> revealed;
    
    auto consume = [&](RawTransaction *tx, TxFile &txFile) -> void {
        auto diskTx = txFile.getData(tx->txNum);
        
        assert(diskTx != nullptr);
        assert(diskTx->inputCount == tx->inputs.size());
        assert(diskTx->outputCount == tx->outputs.size());
        
        uint16_t i = 0;
        
        for (auto &input : tx->inputs) {
            auto spentTx = txFile.getData(input.linkedTxNum);
            auto &spentOutput = spentTx->getOutput(input.rawOutputPointer.outputNum);
            
            assert(spentOutput.toAddressNum > 0);
            
            auto address = spentOutput.getAddress();
            auto &diskInput = diskTx->getInput(i);
            diskInput.toAddressNum = address.addressNum;
            spentOutput.linkedTxNum = tx->txNum;
            
            InputInfo info = input.getInfo(i, tx->txNum, address.addressNum, tx->isSegwit);
            auto processedInput = processInput(address, info, *tx, addressState, addressWriter);
            
            for (auto &index : processedInput) {
                revealed.push_back(index);
            }
            i++;
        }

        i = 0;
        for (auto &output : tx->outputs) {
            auto &diskOutput = diskTx->getOutput(i);
            auto address = processOutput(output.scriptOutput, addressState, addressWriter);
            assert(address.addressNum > 0);
            diskOutput.toAddressNum = address.addressNum;
            i++;
        }
        
        auto currentCount = tx->txNum - startingTxCount;
        if (currentCount % percentageMarker == 0) {
            auto percentDone = (static_cast<double>(currentCount) / static_cast<double>(totalTxCount)) * 100;
            auto blockHeight = tx->blockHeight;
            std::cout << "\r" << percentDone << "% done, Block " << blockHeight << "/" << maxBlockHeight << std::flush;
        }
        
        if (tx->sizeBytes > 800) {
            delete tx;
        } else {
            if (!finished_transaction_queue.push(tx)) {
                delete tx;
            }
        }
        
        addressState.optionalSave();
    };
    
    TxFile txFile{config.txFilePath()};
    RawTransaction *rawTx = nullptr;
    while (!utxoDone) {
        while (address_transaction_queue.read_available() > 10000 && address_transaction_queue.pop(rawTx)) {
            if (rawTx->txNum + 5000 >= txFile.size()) {
                txFile.reload();
            }
            consume(rawTx, txFile);
        }
        std::this_thread::sleep_for(100ms);
    }
    txFile.reload();
    while (address_transaction_queue.pop(rawTx)) {
        consume(rawTx, txFile);
    }
    
    return revealed;
}

template <typename ParseTag>
std::vector<uint32_t> BlockProcessor::addNewBlocks(const ParserConfiguration<ParseTag> &config, std::vector<BlockInfo<ParseTag>> nextBlocks, UTXOState &utxoState, AddressState &addressState) {
    
    rawDone = false;
    hashDone = false;
    utxoDone = false;
    
    auto importer = std::async(std::launch::async, [&] {
        readNewBlocks(config, nextBlocks);
    });
    
    auto hashCalculator = std::async(std::launch::async, [&] {
        calculateHashes(config);
    });
    
    auto utxoProcessor = std::async(std::launch::async, [&] {
        processUTXOs(config, utxoState);
    });
    
    auto addressProcessor = std::async(std::launch::async, [&] {
        return processAddresses(config, addressState);
    });
    
    importer.get();
    hashCalculator.get();
    utxoProcessor.get();
    auto ret = addressProcessor.get();
    files.clear();
    return ret;
}

#ifdef BLOCKSCI_FILE_PARSER
template std::vector<uint32_t> BlockProcessor::addNewBlocks(const ParserConfiguration<FileTag> &config, std::vector<BlockInfo<FileTag>> nextBlocks, UTXOState &utxoState, AddressState &addressState);
#endif
#ifdef BLOCKSCI_RPC_PARSER
template std::vector<uint32_t> BlockProcessor::addNewBlocks(const ParserConfiguration<RPCTag> &config, std::vector<BlockInfo<RPCTag>> nextBlocks, UTXOState &utxoState, AddressState &addressState);
#endif

BlockProcessor::~BlockProcessor() {
    assert(!utxo_transaction_queue.pop());
    assert(!address_transaction_queue.pop());
    
    finished_transaction_queue.consume_all([](RawTransaction *tx) {
        delete tx;
    });
}
