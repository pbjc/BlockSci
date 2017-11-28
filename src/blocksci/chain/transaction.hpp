//
//  transaction.hpp
//  BlockReader
//
//  Created by Harry Kalodner on 1/4/17.
//  Copyright © 2017 Harry Kalodner. All rights reserved.
//

#ifndef transaction_hpp
#define transaction_hpp

#include "raw_transaction.hpp"
#include "output.hpp"
#include "input.hpp"
#include "chain_access.hpp"

#include <blocksci/scripts/scripts_fwd.hpp>
#include <blocksci/address/address_info.hpp>

#include <range/v3/iterator_range.hpp>
#include <range/v3/numeric/accumulate.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/view/remove_if.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/zip_with.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/utility/optional.hpp>

#include <vector>

enum class CoinJoinResult {
    True, False, Timeout
};

namespace blocksci {
    struct Address;
    class uint256;
    class HashIndex;
    
    class Transaction {
    private:
        const ChainAccess *access;
        const RawTransaction *data;
        friend TransactionSummary;
    public:
        uint32_t txNum;
        uint32_t blockHeight;
        
        Transaction() = default;
        
        Transaction(const RawTransaction *data_, uint32_t txNum_, uint32_t blockHeight_, const ChainAccess &access_) : access(&access_), data(data_), txNum(txNum_), blockHeight(blockHeight_) {}
        
        Transaction(uint32_t index, const ChainAccess &access_) : Transaction(index, access_.getBlockHeight(index), access_) {}
        
        Transaction(uint32_t index, uint32_t height, const ChainAccess &access_) : Transaction(access_.getTx(index), index, height, access_) {}
        
        static ranges::optional<Transaction> txWithHash(uint256 hash, const HashIndex &index, const ChainAccess &access);
        static ranges::optional<Transaction> txWithHash(std::string hash, const HashIndex &index, const ChainAccess &access);
        
        uint256 getHash() const;
        std::string getString() const;
        
        std::vector<OutputPointer> getOutputPointers(const InputPointer &pointer) const;
        std::vector<InputPointer> getInputPointers(const OutputPointer &pointer) const;
        
        uint32_t sizeBytes() const {
            return data->sizeBytes;
        }
        
        uint32_t locktime() const {
            return data->locktime;
        }
        
        uint16_t inputCount() const {
            return data->inputCount;
        }
        
        uint16_t outputCount() const {
            return data->outputCount;
        }
        
        ranges::iterator_range<const Inout *> rawOutputs() const {
            auto &firstOut = data->getOutput(0);
            return ranges::make_iterator_range(&firstOut, &firstOut + outputCount());
        }
        
        ranges::iterator_range<const Inout *> rawInputs() const {
            auto &firstIn = data->getInput(0);
            return ranges::make_iterator_range(&firstIn, &firstIn + inputCount());
        }
        
        auto outputs() const {
            auto chainAccess = access;
            uint32_t txIndex = txNum;
            uint32_t height = blockHeight;
            return ranges::view::zip_with([chainAccess, txIndex, height](uint16_t outputNum, const Inout &inout) {
                return Output({txIndex, outputNum}, height, inout, *chainAccess);
            }, ranges::view::iota(uint16_t{0}, outputCount()), rawOutputs());
        }
        
        auto inputs() const {
            auto chainAccess = access;
            uint32_t txIndex = txNum;
            uint32_t height = blockHeight;
            return ranges::view::zip_with([chainAccess, txIndex, height](uint16_t inputNum, const Inout &inout) {
                return Input({txIndex, inputNum}, height, inout, *chainAccess);
            }, ranges::view::iota(uint16_t{0}, inputCount()), rawInputs());
        }
        
        bool isCoinbase() const {
            return inputCount() == 0;
        }
        
        Block block() const;
        
        // Requires DataAccess
        #ifndef BLOCKSCI_WITHOUT_SINGLETON
        Transaction(uint32_t index);
        Transaction(uint32_t index, uint32_t height);
        static ranges::optional<Transaction> txWithHash(uint256 hash);
        static ranges::optional<Transaction> txWithHash(std::string hash);
        #endif
    };
    
    inline bool operator==(const Transaction& a, const Transaction& b) {
        return a.txNum == b.txNum;
    }

    inline bool operator!=(const Transaction& a, const Transaction& b) {
        return a.txNum != b.txNum;
    }
    
    inline bool operator<(const Transaction& a, const Transaction& b) {
        return a.txNum < b.txNum;
    }
    
    using input_range = decltype(std::declval<Transaction>().inputs());
    using output_range = decltype(std::declval<Transaction>().outputs());
    
    bool hasFeeGreaterThan(Transaction &tx, uint64_t fee);
    
    bool isCoinjoin(const Transaction &tx);
    bool isDeanonTx(const Transaction &tx);
    bool isChangeOverTx(const Transaction &tx, const ScriptAccess &scripts);
    bool containsKeysetChange(const Transaction &tx, const blocksci::ScriptAccess &access);
    CoinJoinResult isPossibleCoinjoin(const Transaction &tx, uint64_t minBaseFee, double percentageFee, size_t maxDepth);
    CoinJoinResult isCoinjoinExtra(const Transaction &tx, uint64_t minBaseFee, double percentageFee, size_t maxDepth);
    ranges::optional<Output> getOpReturn(const Transaction &tx);
    
    ranges::optional<Output> getChangeOutput(const Transaction &tx, const ScriptAccess &scripts);
    
    #ifndef BLOCKSCI_WITHOUT_SINGLETON
    bool containsKeysetChange(const Transaction &tx);
    
    ranges::optional<Output> getChangeOutput(const Transaction &tx);
    bool isChangeOverTx(const Transaction &tx);
    
    #endif
}


namespace std {
    template <>
    struct hash<blocksci::Transaction> {
        typedef blocksci::Transaction argument_type;
        typedef size_t  result_type;
        result_type operator()(const argument_type &b) const {
            return static_cast<size_t>(b.txNum);
        }
    };
}


#endif /* transaction_hpp */
