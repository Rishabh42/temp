﻿// Copyright (c) 2017-2018 The Syscoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "services/assetallocation.h"
#include "services/asset.h"
#include "init.h"
#include "validation.h"
#include "txmempool.h"
#include "util.h"
#include "core_io.h"
#include "wallet/wallet.h"
#include "chainparams.h"
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <key_io.h>
#include <future>
#include <rpc/util.h>
// SYSCOIN service rpc functions
extern UniValue sendrawtransaction(const JSONRPCRequest& request);

UniValue assetallocationsend(const JSONRPCRequest& request);
UniValue assetallocationmint(const JSONRPCRequest& request);
UniValue assetallocationburn(const JSONRPCRequest& request);
UniValue assetallocationinfo(const JSONRPCRequest& request);
UniValue assetallocationsenderstatus(const JSONRPCRequest& request);
UniValue listassetallocationtransactions(const JSONRPCRequest& request);
UniValue listassetallocations(const JSONRPCRequest& request);
UniValue tpstestinfo(const JSONRPCRequest& request);
UniValue tpstestadd(const JSONRPCRequest& request);
UniValue tpstestsetenabled(const JSONRPCRequest& request);
UniValue listassetallocationmempoolbalances(const JSONRPCRequest& request);

using namespace std;
AssetAllocationIndexItemMap AssetAllocationIndex GUARDED_BY(cs_assetallocationindex);
AssetBalanceMap mempoolMapAssetBalances GUARDED_BY(cs_assetallocation);
ArrivalTimesMapImpl arrivalTimesMap GUARDED_BY(cs_assetallocationarrival);

string CWitnessAddress::ToString() const {
    if (vchWitnessProgram.size() <= 4 && stringFromVch(vchWitnessProgram) == "burn")
        return "burn";
    
    if(nVersion == 0){
        if (vchWitnessProgram.size() == WITNESS_V0_KEYHASH_SIZE) {
            return EncodeDestination(WitnessV0KeyHash(vchWitnessProgram));
        }
        else if (vchWitnessProgram.size() == WITNESS_V0_SCRIPTHASH_SIZE) {
            return EncodeDestination(WitnessV0ScriptHash(vchWitnessProgram));
        }
    }
    return "";
}
bool CWitnessAddress::IsValid() const {
    const size_t& size = vchWitnessProgram.size();
    // this is a hard limit 2->40
    if(size < 2 || size > 40){
        return false;
    }
    // BIP 142, version 0 must be of p2wpkh or p2wpsh size
    if(nVersion == 0){
        return (size == WITNESS_V0_KEYHASH_SIZE || size == WITNESS_V0_SCRIPTHASH_SIZE);
    }
    // otherwise mark as valid for future softfork expansion
    return true;
}
string CAssetAllocationTuple::ToString() const {
	return boost::lexical_cast<string>(nAsset) + "-" + witnessAddress.ToString();
}
string assetAllocationFromOp(int op) {
    switch (op) {
	case OP_ASSET_SEND:
		return "assetsend";
	case OP_ASSET_ALLOCATION_SEND:
		return "assetallocationsend";
	case OP_ASSET_ALLOCATION_BURN:
		return "assetallocationburn";      
    default:
        return "<unknown assetallocation op>";
    }
}
bool CAssetAllocation::UnserializeFromData(const vector<unsigned char> &vchData) {
    try {
        CDataStream dsAsset(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsAsset >> *this;
    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	return true;
}
bool CAssetAllocation::UnserializeFromTx(const CTransaction &tx) {
	vector<unsigned char> vchData;
	int nOut, op;
	if (!GetSyscoinData(tx, vchData, nOut, op) || op != OP_SYSCOIN_ASSET_ALLOCATION)
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData))
	{	
		return false;
	}
    return true;
}
void CAssetAllocation::Serialize( vector<unsigned char> &vchData) {
    CDataStream dsAsset(SER_NETWORK, PROTOCOL_VERSION);
    dsAsset << *this;
	vchData = vector<unsigned char>(dsAsset.begin(), dsAsset.end());

}
void CAssetAllocationDB::WriteMintIndex(const CTransaction& tx, const CMintSyscoin& mintSyscoin, const int &nHeight){
    if (fZMQAssetAllocation || fAssetAllocationIndex) {
        UniValue output(UniValue::VOBJ);
        AssetMintTxToJson(tx, mintSyscoin, nHeight, output);
        GetMainSignals().NotifySyscoinUpdate(output.write().c_str(), "assetallocation");
    }
}
void CAssetAllocationDB::WriteAssetAllocationIndex(const int& op, const CTransaction &tx, const CAsset& dbAsset, const bool& confirmed, int nHeight) {
	if (fZMQAssetAllocation || fAssetAllocationIndex) {
		UniValue oName(UniValue::VOBJ);
        string strSender;
        bool isMine = AssetAllocationTxToJSON(op, tx, dbAsset, nHeight, confirmed, oName, strSender);
        const string& strObj = oName.write();
        GetMainSignals().NotifySyscoinUpdate(strObj.c_str(), "assetallocation");
		if (isMine && fAssetAllocationIndex) {
            const string& txHash = tx.GetHash().GetHex();
			const string& strKey = txHash+"-"+boost::lexical_cast<string>(dbAsset.nAsset)+"-"+ strSender;
			{
				LOCK2(mempool.cs, cs_assetallocationindex);
				// we want to the height from mempool if it exists or use the one passed in
				CTxMemPool::txiter it = mempool.mapTx.find(tx.GetHash());
				if (it != mempool.mapTx.end())
					nHeight = (*it).GetHeight();
				AssetAllocationIndex[nHeight][strKey] = strObj;
			}
		}
	}

}
bool GetAssetAllocation(const CAssetAllocationTuple &assetAllocationTuple, CAssetAllocation& txPos) {
    if (passetallocationdb == nullptr || !passetallocationdb->ReadAssetAllocation(assetAllocationTuple, txPos))
        return false;
    return true;
}
bool DecodeAndParseAssetAllocationTx(const CTransaction& tx, int& op,
		vector<vector<unsigned char> >& vvch, char &type)
{
	CAssetAllocation assetallocation;
	// parse asset allocation, if not try to parse asset
	bool decode = DecodeAssetAllocationTx(tx, op, vvch)? true: DecodeAssetTx(tx, op, vvch);
	bool parse = assetallocation.UnserializeFromTx(tx);
	if (decode&&parse) {
		type = OP_SYSCOIN_ASSET_ALLOCATION;
		return true;
	}
	return false;
}
bool DecodeAssetAllocationTx(const CTransaction& tx, int& op,
        vector<vector<unsigned char> >& vvch) {
    bool found = false;

    // Strict check - bug disallowed
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& out = tx.vout[i];
        vector<vector<unsigned char> > vvchRead;
        if (DecodeAssetAllocationScript(out.scriptPubKey, op, vvchRead)) {
            found = true; vvch = vvchRead;
            break;
        }
    }
    if (!found) {vvch.clear(); op= 0;}
    return found;
}

void ResyncAssetAllocationStates(){ 
    int count = 0;
     {
        
        vector<string> vecToRemoveMempoolBalances;
        LOCK2(cs_main, mempool.cs);
        LOCK(cs_assetallocation);
        LOCK(cs_assetallocationarrival);
        for (auto&indexObj : mempoolMapAssetBalances) {
            vector<uint256> vecToRemoveArrivalTimes;
            const string& strSenderTuple = indexObj.first;
            // if no arrival time for this mempool balance, remove it
            auto arrivalTimes = arrivalTimesMap.find(strSenderTuple);
            if(arrivalTimes == arrivalTimesMap.end()){
                vecToRemoveMempoolBalances.push_back(strSenderTuple);
                continue;
            }
            for(auto& arrivalTime: arrivalTimes->second){
                const uint256& txHash = arrivalTime.first;
                const CTransactionRef txRef = mempool.get(txHash);
                // if mempool doesnt have txid then remove from both arrivalTime and mempool balances
                if (!txRef){
                    vecToRemoveArrivalTimes.push_back(txHash);
                }
                if(!arrivalTime.first.IsNull() && ((chainActive.Tip()->GetMedianTimePast()*1000) - arrivalTime.second) > 1800000){
                    vecToRemoveArrivalTimes.push_back(txHash);
                }
            }
            // if we are removing everything from arrivalTime map then might as well remove it from parent altogether
            if(vecToRemoveArrivalTimes.size() >= arrivalTimes->second.size()){
                arrivalTimesMap.erase(strSenderTuple);
                vecToRemoveMempoolBalances.push_back(strSenderTuple);
            } 
            // otherwise remove the individual txids
            else{
                for(auto &removeTxHash: vecToRemoveArrivalTimes){
                    arrivalTimes->second.erase(removeTxHash);
                }
            }         
        }
        count+=vecToRemoveMempoolBalances.size();
        for(auto& senderTuple: vecToRemoveMempoolBalances){
            mempoolMapAssetBalances.erase(senderTuple);
            // also remove from assetAllocationConflicts
            sorted_vector<string>::const_iterator it = assetAllocationConflicts.find(senderTuple);
            if (it != assetAllocationConflicts.end()) {
                assetAllocationConflicts.V.erase(const_iterator_cast(assetAllocationConflicts.V, it));
            }
        }       
    }   
    if(count > 0)
        LogPrint(BCLog::SYS,"removeExpiredMempoolBalances removed %d expired asset allocation transactions from mempool balances\n", count);

}
bool ResetAssetAllocation(const string &senderStr, const uint256 &txHash, const bool &bMiner, const bool& bCheckExpiryOnly) {
    bool removeAllConflicts = true;
    if(!bMiner){
        {
            LOCK(cs_assetallocationarrival);
        	// remove the conflict once we revert since it is assumed to be resolved on POW
        	auto arrivalTimes = arrivalTimesMap.find(senderStr);
            
        	if(arrivalTimes != arrivalTimesMap.end()){
            	// remove only if all arrival times are either expired (30 mins) or no more zdag transactions left for this sender
            	for(auto& arrivalTime: arrivalTimes->second){
                    // ensure mempool has the tx and its less than 30 mins old
                    if(bCheckExpiryOnly && !mempool.get(arrivalTime.first))
                        continue;
            		if(!arrivalTime.first.IsNull() && ((chainActive.Tip()->GetMedianTimePast()*1000) - arrivalTime.second) <= 1800000){
            			removeAllConflicts = false;
            			break;
            		}
            	}
            }
        	if(removeAllConflicts){
                if(arrivalTimes != arrivalTimesMap.end())
                    arrivalTimesMap.erase(arrivalTimes);
                sorted_vector<string>::const_iterator it = assetAllocationConflicts.find(senderStr);
                if (it != assetAllocationConflicts.end()) {
                    assetAllocationConflicts.V.erase(const_iterator_cast(assetAllocationConflicts.V, it));
                }   
        	}
            else if(!bCheckExpiryOnly){
                arrivalTimes->second.erase(txHash);
                if(arrivalTimes->second.size() <= 0)
                    removeAllConflicts = true;
            }
        }
        if(removeAllConflicts)
        {
            LOCK(cs_assetallocation);
            mempoolMapAssetBalances.erase(senderStr);
        }
    }
	

	return removeAllConflicts;
	
}
bool DisconnectMintAsset(const CTransaction &tx, AssetMap &mapAssets, AssetAllocationMap &mapAssetAllocations){
    CMintSyscoin mintSyscoin(tx);
    CAsset dbAsset;
    if(mintSyscoin.IsNull())
    {
        LogPrint(BCLog::SYS,"DisconnectSyscoinTransaction: Cannot unserialize data inside of this transaction relating to an syscoinmint\n");
        return false;
    }
    auto result = mapAssets.try_emplace(mintSyscoin.assetAllocationTuple.nAsset,std::move(emptyAsset));
    auto mapAsset = result.first;
    const bool& mapAssetNotFound = result.second;
    if(mapAssetNotFound){
        if (!GetAsset(mintSyscoin.assetAllocationTuple.nAsset, dbAsset)) {
            LogPrint(BCLog::SYS,"DisconnectSyscoinTransaction: Could not get asset %d\n",mintSyscoin.assetAllocationTuple.nAsset);
            return false;               
        } 
        mapAsset->second = std::move(dbAsset);                   
    }
    CAsset& storedSenderRef = mapAsset->second;    
 
    const std::string &receiverTupleStr = mintSyscoin.assetAllocationTuple.ToString();
    auto result1 = mapAssetAllocations.try_emplace(std::move(receiverTupleStr), std::move(emptyAllocation));
    auto mapAssetAllocation = result1.first;
    const bool& mapAssetAllocationNotFound = result1.second;
    if(mapAssetAllocationNotFound){
        CAssetAllocation receiverAllocation;
        GetAssetAllocation(mintSyscoin.assetAllocationTuple, receiverAllocation);
        if (receiverAllocation.assetAllocationTuple.IsNull()) {
            receiverAllocation.assetAllocationTuple.nAsset = std::move(mintSyscoin.assetAllocationTuple.nAsset);
            receiverAllocation.assetAllocationTuple.witnessAddress = std::move(mintSyscoin.assetAllocationTuple.witnessAddress);
        } 
        mapAssetAllocation->second = std::move(receiverAllocation);                 
    }
    CAssetAllocation& storedReceiverAllocationRef = mapAssetAllocation->second;
       
    storedReceiverAllocationRef.nBalance -= mintSyscoin.nValueAsset;
    storedSenderRef.nTotalSupply -= mintSyscoin.nValueAsset;
    if(dbAsset.nTotalSupply < 0){
        LogPrint(BCLog::SYS,"DisconnectSyscoinTransaction: Max supply cannot be negative\n");
        return false;
    }  
    if(storedReceiverAllocationRef.nBalance < 0) {
        LogPrint(BCLog::SYS,"DisconnectSyscoinTransaction: Receiver balance of %s is negative: %lld\n",mintSyscoin.assetAllocationTuple.ToString(), storedReceiverAllocationRef.nBalance);
        return false;
    }       
    else if(storedReceiverAllocationRef.nBalance == 0){
        storedReceiverAllocationRef.SetNull();
    }
    
    return true; 
}
bool DisconnectAssetAllocation(const CTransaction &tx, AssetAllocationMap &mapAssetAllocations){
    CAssetAllocation theAssetAllocation(tx);
    const std::string &senderTupleStr = theAssetAllocation.assetAllocationTuple.ToString();
    if(theAssetAllocation.assetAllocationTuple.IsNull()){
        LogPrint(BCLog::SYS,"DisconnectSyscoinTransaction: Could not decode asset allocation\n");
        return false;
    }
    auto result = mapAssetAllocations.try_emplace(std::move(senderTupleStr), std::move(emptyAllocation));
    auto mapAssetAllocation = result.first;
    const bool & mapAssetAllocationNotFound = result.second;
    if(mapAssetAllocationNotFound){
        CAssetAllocation senderAllocation;
        GetAssetAllocation(theAssetAllocation.assetAllocationTuple, senderAllocation);
        if (senderAllocation.assetAllocationTuple.IsNull()) {
            senderAllocation.assetAllocationTuple.nAsset = std::move(theAssetAllocation.assetAllocationTuple.nAsset);
            senderAllocation.assetAllocationTuple.witnessAddress = std::move(theAssetAllocation.assetAllocationTuple.witnessAddress);       
        } 
        mapAssetAllocation->second = std::move(senderAllocation);                 
    }
    CAssetAllocation& storedSenderAllocationRef = mapAssetAllocation->second;

    for(const auto& amountTuple:theAssetAllocation.listSendingAllocationAmounts){
        const CAssetAllocationTuple receiverAllocationTuple(theAssetAllocation.assetAllocationTuple.nAsset, amountTuple.first);
       
        const std::string &receiverTupleStr = receiverAllocationTuple.ToString();
        CAssetAllocation receiverAllocation;
        
        auto result1 = mapAssetAllocations.try_emplace(std::move(receiverTupleStr), std::move(emptyAllocation));
        auto mapAssetAllocationReceiver = result1.first;
        const bool& mapAssetAllocationReceiverNotFound = result1.second;
        if(mapAssetAllocationReceiverNotFound){
            GetAssetAllocation(receiverAllocationTuple, receiverAllocation);
            if (receiverAllocation.assetAllocationTuple.IsNull()) {
                receiverAllocation.assetAllocationTuple.nAsset = std::move(receiverAllocationTuple.nAsset);
                receiverAllocation.assetAllocationTuple.witnessAddress = std::move(receiverAllocationTuple.witnessAddress);
            } 
            mapAssetAllocationReceiver->second = std::move(receiverAllocation);                 
        }
        CAssetAllocation& storedReceiverAllocationRef = mapAssetAllocationReceiver->second;

        // reverse allocations
        storedReceiverAllocationRef.nBalance -= amountTuple.second;
        storedSenderAllocationRef.nBalance += amountTuple.second; 
        if(storedReceiverAllocationRef.nBalance < 0) {
            LogPrint(BCLog::SYS,"DisconnectSyscoinTransaction: Receiver balance of %s is negative: %lld\n",receiverAllocationTuple.ToString(), storedReceiverAllocationRef.nBalance);
            return false;
        }
        else if(storedReceiverAllocationRef.nBalance == 0){
            storedReceiverAllocationRef.SetNull();  
        }                              
    }
    return true; 
}
bool CheckAssetAllocationInputs(const CTransaction &tx, const CCoinsViewCache &inputs, int op, const vector<vector<unsigned char> > &vvchArgs,
        bool fJustCheck, int nHeight, AssetAllocationMap &mapAssetAllocations, string &errorMessage, bool& bOverflow, bool bSanityCheck, bool bMiner) {
    if (passetallocationdb == nullptr)
		return false;
	const uint256 & txHash = tx.GetHash();
	if (!bSanityCheck)
		LogPrint(BCLog::SYS,"*** ASSET ALLOCATION %d %d %s %s\n", nHeight,
			chainActive.Tip()->nHeight, txHash.ToString().c_str(),
			fJustCheck ? "JUSTCHECK" : "BLOCK");
            

	// unserialize assetallocation from txn, check for valid
	CAssetAllocation theAssetAllocation;
	vector<unsigned char> vchData;
	int nDataOut, type;
	if(!GetSyscoinData(tx, vchData, nDataOut, type) || type != OP_SYSCOIN_ASSET_ALLOCATION || !theAssetAllocation.UnserializeFromData(vchData))
	{
		errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR ERRCODE: 1001 - " + _("Cannot unserialize data inside of this transaction relating to an assetallocation");
		return error(errorMessage.c_str());
	}

	if(fJustCheck)
	{
		if(op == OP_ASSET_ALLOCATION_BURN && vvchArgs.size() != 4)
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1002 - " + _("Asset arguments incorrect size");
			return error(errorMessage.c_str());
		}		
		
	}
	string retError = "";
	if(fJustCheck)
	{
		switch (op) {
		case OP_ASSET_ALLOCATION_SEND:
			if (theAssetAllocation.listSendingAllocationAmounts.empty())
			{
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1004 - " + _("Asset send must send an input or transfer balance");
				return error(errorMessage.c_str());
			}
			if (theAssetAllocation.listSendingAllocationAmounts.size() > 250)
			{
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1005 - " + _("Too many receivers in one allocation send, maximum of 250 is allowed at once");
				return error(errorMessage.c_str());
			}
			break;
		case OP_ASSET_ALLOCATION_BURN:
			if (theAssetAllocation.listSendingAllocationAmounts.size() != 1)
			{
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1007 - " + _("Must send exactly one output to burn transaction");
				return error(errorMessage.c_str());
			}
			break;           
		default:
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1009 - " + _("Asset transaction has unknown op");
			return error(errorMessage.c_str());
		}
	}

	const CWitnessAddress &user1 = theAssetAllocation.assetAllocationTuple.witnessAddress;
    const string & senderTupleStr = theAssetAllocation.assetAllocationTuple.ToString();

	CAssetAllocation dbAssetAllocation;
    AssetAllocationMap::iterator mapAssetAllocation;
	CAsset dbAsset;
    if(fJustCheck){
        if (!GetAssetAllocation(theAssetAllocation.assetAllocationTuple, dbAssetAllocation))
        {
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1010 - " + _("Cannot find sender asset allocation");
            return error(errorMessage.c_str());
        }     
    }
    else{
        auto result = mapAssetAllocations.try_emplace(senderTupleStr, std::move(emptyAllocation));
        mapAssetAllocation = result.first;
        const bool& mapAssetAllocationNotFound = result.second;
        
        if(mapAssetAllocationNotFound){
            if (!GetAssetAllocation(theAssetAllocation.assetAllocationTuple, dbAssetAllocation))
            {
                errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1010 - " + _("Cannot find sender asset allocation");
                return error(errorMessage.c_str());
            }        
            mapAssetAllocation->second = std::move(dbAssetAllocation);                   
        }
    }
    CAssetAllocation& storedSenderAllocationRef = fJustCheck? dbAssetAllocation:mapAssetAllocation->second;
    
    if (!GetAsset(storedSenderAllocationRef.assetAllocationTuple.nAsset, dbAsset))
    {
        errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1011 - " + _("Failed to read from asset DB");
        return error(errorMessage.c_str());
    }
        
    AssetBalanceMap::iterator mapBalanceSender;
    CAmount mapBalanceSenderCopy;
    bool mapSenderMempoolBalanceNotFound = false;
    if(fJustCheck){
        LOCK(cs_assetallocation); 
        auto result = mempoolMapAssetBalances.try_emplace(senderTupleStr, std::move(storedSenderAllocationRef.nBalance));
        mapBalanceSender = result.first;
        mapSenderMempoolBalanceNotFound = result.second;
        mapBalanceSenderCopy = mapBalanceSender->second;
    }
    else
        mapBalanceSenderCopy = storedSenderAllocationRef.nBalance;     
           
	if (op == OP_ASSET_ALLOCATION_BURN)
	{		
		if (storedSenderAllocationRef.assetAllocationTuple != theAssetAllocation.assetAllocationTuple || !FindAssetOwnerInTx(inputs, tx, user1))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1015 - " + _("Cannot send this asset. Asset allocation owner must sign off on this change");
			return error(errorMessage.c_str());
		}
        if(vvchArgs[0].size() != 4)
        {
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1010 - " + _("Invalid asset details entered in the script output");
            return error(errorMessage.c_str());
        }
        uint32_t nAssetFromScript  = static_cast<uint32_t>(vvchArgs[0][3]);
        nAssetFromScript |= static_cast<uint32_t>(vvchArgs[0][2]) << 8;
        nAssetFromScript |= static_cast<uint32_t>(vvchArgs[0][1]) << 16;
        nAssetFromScript |= static_cast<uint32_t>(vvchArgs[0][0]) << 24;
		if(storedSenderAllocationRef.assetAllocationTuple.nAsset != nAssetFromScript)
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1010 - " + _("Invalid asset details entered in the script output");
			return error(errorMessage.c_str());
		}	


		if(dbAsset.vchContract.empty() || dbAsset.vchContract != vvchArgs[2])
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1010 - " + _("Invalid contract entered in the script output or Syscoin Asset does not have the ethereum contract configured");
			return error(errorMessage.c_str());
		}
        const auto &amountTuple = theAssetAllocation.listSendingAllocationAmounts[0];
        if (amountTuple.second <= 0)
        {
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1020 - " + _("Burning amount must be positive");
            return error(errorMessage.c_str());
        } 
        if(vvchArgs[1].size() != 8)
        {
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1010 - " + _("Invalid amount entered in the script output");
            return error(errorMessage.c_str());
        }       
        uint64_t nAmountFromScript  = static_cast<uint64_t>(vvchArgs[1][7]);
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][6]) << 8;
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][5]) << 16;
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][4]) << 24; 
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][3]) << 32;  
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][2]) << 40;  
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][1]) << 48;  
        nAmountFromScript |= static_cast<uint64_t>(vvchArgs[1][0]) << 56; 
        if((CAmount)nAmountFromScript != amountTuple.second)
        {
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1015 - " + _("Invalid amount entered in the script output");
            return error(errorMessage.c_str());
        }   
        if(amountTuple.first.ToString() != "burn")
        {
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1015 - " + _("Must send output to burn address");
            return error(errorMessage.c_str());
        } 
        mapBalanceSenderCopy -= amountTuple.second;
		if (mapBalanceSenderCopy < 0) {
            if(fJustCheck)
            {
                LOCK(cs_assetallocation); 
                if(mapSenderMempoolBalanceNotFound){
                    mempoolMapAssetBalances.erase(mapBalanceSender);
                }
            }
            bOverflow = true;
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1016 - " + _("Sender balance is insufficient");
			return error(errorMessage.c_str());
		}
		if (!fJustCheck) {   
            const CAssetAllocationTuple receiverAllocationTuple(theAssetAllocation.assetAllocationTuple.nAsset, amountTuple.first);
            const string& receiverTupleStr = receiverAllocationTuple.ToString();  
            auto result = mapAssetAllocations.try_emplace(receiverTupleStr, std::move(emptyAllocation));
            auto mapAssetAllocationReceiver = result.first;
            const bool& mapAssetAllocationReceiverNotFound = result.second;
            if(mapAssetAllocationReceiverNotFound){
                CAssetAllocation dbAssetAllocationReceiver;
                if (!GetAssetAllocation(receiverAllocationTuple, dbAssetAllocationReceiver)) {
                    dbAssetAllocationReceiver.assetAllocationTuple.nAsset = std::move(receiverAllocationTuple.nAsset);
                    dbAssetAllocationReceiver.assetAllocationTuple.witnessAddress = std::move(receiverAllocationTuple.witnessAddress);               
                }
                mapAssetAllocationReceiver->second = std::move(dbAssetAllocationReceiver);                   
            } 
            mapAssetAllocationReceiver->second.nBalance += amountTuple.second;                        
        }else if (!bSanityCheck) {
            LOCK(cs_assetallocationarrival);
            // add conflicting sender if using ZDAG
            assetAllocationConflicts.insert(senderTupleStr);
        }
	}
	else if (op == OP_ASSET_ALLOCATION_SEND)
	{
		if (storedSenderAllocationRef.assetAllocationTuple != theAssetAllocation.assetAllocationTuple || !FindAssetOwnerInTx(inputs, tx, user1))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1015a - " + _("Cannot send this asset. Asset allocation owner must sign off on this change");
			return error(errorMessage.c_str());
		}	    
		
		// check balance is sufficient on sender
		CAmount nTotal = 0;
		for (const auto& amountTuple : theAssetAllocation.listSendingAllocationAmounts) {
			nTotal += amountTuple.second;
			if (amountTuple.second <= 0)
			{
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1020 - " + _("Receiving amount must be positive");
				return error(errorMessage.c_str());
			}
		}
        mapBalanceSenderCopy -= nTotal;
		if (mapBalanceSenderCopy < 0) {
            if(fJustCheck && !bSanityCheck)
            {
                LOCK(cs_assetallocationarrival);
				// add conflicting sender
				assetAllocationConflicts.insert(senderTupleStr);
            }
            if(fJustCheck)
            {
                LOCK(cs_assetallocation); 
                if(mapSenderMempoolBalanceNotFound){
                    mempoolMapAssetBalances.erase(mapBalanceSender);
                }
            }
            bOverflow = true;            
            errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1021 - " + _("Sender balance is insufficient");
            return error(errorMessage.c_str());
		}
		       
		for (const auto& amountTuple : theAssetAllocation.listSendingAllocationAmounts) {
			if (amountTuple.first == theAssetAllocation.assetAllocationTuple.witnessAddress) {
                if(fJustCheck)
                {
                    LOCK(cs_assetallocation); 
                    if(mapSenderMempoolBalanceNotFound){
                        mempoolMapAssetBalances.erase(mapBalanceSender);
                    }
                }           
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1022 - " + _("Cannot send an asset allocation to yourself");
				return error(errorMessage.c_str());
			}
		
			const CAssetAllocationTuple receiverAllocationTuple(theAssetAllocation.assetAllocationTuple.nAsset, amountTuple.first);
            const string &receiverTupleStr = receiverAllocationTuple.ToString();
            AssetBalanceMap::iterator mapBalanceReceiver;
            AssetAllocationMap::iterator mapBalanceReceiverBlock;            
            if(fJustCheck){
                LOCK(cs_assetallocation);
                auto result = mempoolMapAssetBalances.try_emplace(std::move(receiverTupleStr), 0);
                auto mapBalanceReceiver = result.first;
                const bool& mapAssetAllocationReceiverNotFound = result.second;
                if(mapAssetAllocationReceiverNotFound){
                    CAssetAllocation receiverAllocation;
                    GetAssetAllocation(receiverAllocationTuple, receiverAllocation);
                    mapBalanceReceiver->second = std::move(receiverAllocation.nBalance);
                }
                if(!bSanityCheck){
                    mapBalanceReceiver->second += amountTuple.second;
                }
            }  
            else{           
                auto result = mapAssetAllocations.try_emplace( receiverTupleStr, std::move(emptyAllocation));
                auto mapBalanceReceiverBlock = result.first;
                const bool& mapAssetAllocationReceiverBlockNotFound = result.second;
                if(mapAssetAllocationReceiverBlockNotFound){
                    CAssetAllocation receiverAllocation;
                    if (!GetAssetAllocation(receiverAllocationTuple, receiverAllocation)) {
                        receiverAllocation.assetAllocationTuple.nAsset = std::move(receiverAllocationTuple.nAsset);
                        receiverAllocation.assetAllocationTuple.witnessAddress = std::move(receiverAllocationTuple.witnessAddress); 
                    }
                    mapBalanceReceiverBlock->second = std::move(receiverAllocation);   
                }
                mapBalanceReceiverBlock->second.nBalance += amountTuple.second; 
                // to remove mempool balances but need to check to ensure that all txid's from arrivalTimes are first gone before removing receiver mempool balance
                // otherwise one can have a conflict as a sender and send himself an allocation and clear the mempool balance inadvertently
                ResetAssetAllocation(receiverTupleStr, txHash, bMiner);           
            }

		} 	
	}
	// write assetallocation  
	// asset sends are the only ones confirming without PoW
    if(!fJustCheck){
        if (!bSanityCheck) {
            ResetAssetAllocation(senderTupleStr, txHash, bMiner);
           
        } 
        storedSenderAllocationRef.listSendingAllocationAmounts.clear();
        storedSenderAllocationRef.nBalance = std::move(mapBalanceSenderCopy);
        if(storedSenderAllocationRef.nBalance == 0)
            storedSenderAllocationRef.SetNull();    

            
        // send notification on pow, for zdag transactions this is the second notification meaning the zdag tx has been confirmed
        passetallocationdb->WriteAssetAllocationIndex(op, tx, dbAsset, true, nHeight);    
        
        LogPrint(BCLog::SYS,"CONNECTED ASSET ALLOCATION: op=%s assetallocation=%s hash=%s height=%d fJustCheck=%d\n",
                assetAllocationFromOp(op).c_str(),
                senderTupleStr.c_str(),
                txHash.ToString().c_str(),
                nHeight,
                fJustCheck ? 1 : 0);                
    }
	else{
        if(!bSanityCheck){
            LOCK(cs_assetallocationarrival);
            ArrivalTimesMap &arrivalTimes = arrivalTimesMap[senderTupleStr];
            arrivalTimes[txHash] = GetTimeMillis();
        }
        if(!bSanityCheck)
        {
            // only send a realtime notification on zdag, send another when pow happens (above)
            if(op == OP_ASSET_ALLOCATION_SEND)
                passetallocationdb->WriteAssetAllocationIndex(op, tx, dbAsset, false, nHeight);
            {
                LOCK(cs_assetallocation);
                mapBalanceSender->second = std::move(mapBalanceSenderCopy);
            }
        }
    } 
    return true;
}
UniValue tpstestinfo(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 0 != params.size())
		throw runtime_error("tpstestinfo\n"
			"Gets TPS Test information for receivers of assetallocation transfers\n");
	if(!fTPSTest)
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1501 - " + _("This function requires tpstest configuration to be set upon startup. Please shutdown and enable it by adding it to your syscoin.conf file and then call 'tpstestsetenabled true'."));
	
	UniValue oTPSTestResults(UniValue::VOBJ);
	UniValue oTPSTestReceivers(UniValue::VARR);
	UniValue oTPSTestReceiversMempool(UniValue::VARR);
	oTPSTestResults.pushKV("enabled", fTPSTestEnabled);
    oTPSTestResults.pushKV("testinitiatetime", (int64_t)nTPSTestingStartTime);
	oTPSTestResults.pushKV("teststarttime", (int64_t)nTPSTestingSendRawEndTime);
   
	for (auto &receivedTime : vecTPSTestReceivedTimesMempool) {
		UniValue oTPSTestStatusObj(UniValue::VOBJ);
		oTPSTestStatusObj.pushKV("txid", receivedTime.first.GetHex());
		oTPSTestStatusObj.pushKV("time", receivedTime.second);
		oTPSTestReceiversMempool.push_back(oTPSTestStatusObj);
	}
	oTPSTestResults.pushKV("receivers", oTPSTestReceiversMempool);
	return oTPSTestResults;
}
UniValue tpstestsetenabled(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 1 != params.size())
		throw runtime_error("tpstestsetenabled [enabled]\n"
			"\nSet TPS Test to enabled/disabled state. Must have -tpstest configuration set to make this call.\n"
			"\nArguments:\n"
			"1. enabled                  (boolean, required) TPS Test enabled state. Set to true for enabled and false for disabled.\n"
			"\nExample:\n"
			+ HelpExampleCli("tpstestsetenabled", "true"));
	if(!fTPSTest)
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1501 - " + _("This function requires tpstest configuration to be set upon startup. Please shutdown and enable it by adding it to your syscoin.conf file and then try again."));
	fTPSTestEnabled = params[0].get_bool();
	if (!fTPSTestEnabled) {
		vecTPSTestReceivedTimesMempool.clear();
		nTPSTestingSendRawEndTime = 0;
		nTPSTestingStartTime = 0;
	}
	UniValue result(UniValue::VOBJ);
	result.pushKV("status", "success");
	return result;
}
UniValue tpstestadd(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 1 > params.size() || params.size() > 2)
		throw runtime_error("tpstestadd [starttime] [{\"tx\":\"hex\"},...]\n"
			"\nAdds raw transactions to the test raw tx queue to be sent to the network at starttime.\n"
			"\nArguments:\n"
			"1. starttime                  (numeric, required) Unix epoch time in micro seconds for when to send the raw transaction queue to the network. If set to 0, will not send transactions until you call this function again with a defined starttime.\n"
			"2. \"raw transactions\"                (array, not-required) A json array of signed raw transaction strings\n"
			"     [\n"
			"       {\n"
			"         \"tx\":\"hex\",    (string, required) The transaction hex\n"
			"       } \n"
			"       ,...\n"
			"     ]\n"
			"\nExample:\n"
			+ HelpExampleCli("tpstestadd", "\"223233433839384\" \"[{\\\"tx\\\":\\\"first raw hex tx\\\"},{\\\"tx\\\":\\\"second raw hex tx\\\"}]\""));
	if (!fTPSTest)
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1501 - " + _("This function requires tpstest configuration to be set upon startup. Please shutdown and enable it by adding it to your syscoin.conf file and then call 'tpstestsetenabled true'."));

	bool bFirstTime = vecTPSRawTransactions.empty();
	nTPSTestingStartTime = params[0].get_int64();
	UniValue txs;
	if(params.size() > 1)
		txs = params[1].get_array();
	if (fTPSTestEnabled) {
		for (unsigned int idx = 0; idx < txs.size(); idx++) {
			const UniValue& tx = txs[idx];
			UniValue paramsRawTx(UniValue::VARR);
			paramsRawTx.push_back(find_value(tx.get_obj(), "tx").get_str());

			JSONRPCRequest request;
			request.params = paramsRawTx;
			vecTPSRawTransactions.push_back(request);
		}
		if (bFirstTime) {
			// define a task for the worker to process
			std::packaged_task<void()> task([]() {
				while (nTPSTestingStartTime <= 0 || GetTimeMicros() < nTPSTestingStartTime) {
					MilliSleep(0);
				}
				nTPSTestingSendRawStartTime = nTPSTestingStartTime;

				for (auto &txReq : vecTPSRawTransactions) {
					sendrawtransaction(txReq);
				}
			});
			bool isThreadPosted = false;
			for (int numTries = 1; numTries <= 50; numTries++)
			{
				// send task to threadpool pointer from init.cpp
				isThreadPosted = threadpool->tryPost(task);
				if (isThreadPosted)
				{
					break;
				}
				MilliSleep(10);
			}
			if (!isThreadPosted)
				throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1501 - " + _("thread pool queue is full"));
		}
	}
	UniValue result(UniValue::VOBJ);
	result.pushKV("status", "success");
	return result;
}
template <typename T>
inline std::string int_to_hex(T val, size_t width=sizeof(T)*2)
{
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(width) << std::hex << (val|0);
    return ss.str();
}
UniValue assetallocationburn(const JSONRPCRequest& request) {
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
	const UniValue &params = request.params;
	if (request.fHelp || 4 != params.size())
		throw runtime_error(
			"assetallocationburn [asset] [address] [amount] [ethereum_destination_address]\n"
			"<asset> Asset guid.\n"
			"<address> Address that owns this asset allocation.\n"
			"<amount> Amount of asset to burn to SYSX.\n"
            "<ethereum_destination_address> The 20 byte (40 character) hex string of the ethereum destination address.\n"
			+ HelpRequiringPassphrase(pwallet));

	const int &nAsset = params[0].get_int();
	string strAddress = params[1].get_str();

	string strAddressFrom;
	const CTxDestination &addressFrom = DecodeDestination(strAddress);
	strAddressFrom = strAddress;
    
    UniValue detail = DescribeAddress(addressFrom);
    if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
        throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
    string witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str();
    unsigned char witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();            	
	CAssetAllocation theAssetAllocation;
	const CAssetAllocationTuple assetAllocationTuple(nAsset, CWitnessAddress(witnessVersion, ParseHex(witnessProgramHex)));
	if (!GetAssetAllocation(assetAllocationTuple, theAssetAllocation))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1500 - " + _("Could not find a asset allocation with this key"));
    {
        LOCK(cs_assetallocationarrival);
        // check to see if a transaction for this asset/address tuple has arrived before minimum latency period
        const ArrivalTimesMap &arrivalTimes = arrivalTimesMap[assetAllocationTuple.ToString()];
        const int64_t & nNow = GetTimeMillis();
        int minLatency = ZDAG_MINIMUM_LATENCY_SECONDS * 1000;
        if (fUnitTest)
            minLatency = 1000;
        for (auto& arrivalTime : arrivalTimes) {
            // if this tx arrived within the minimum latency period flag it as potentially conflicting
            if ((nNow - arrivalTime.second) < minLatency) {
                throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1503 - " + _("Please wait a few more seconds and try again..."));
            }
        }
    }
	CAsset theAsset;
	if (!GetAsset(nAsset, theAsset))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1501 - " + _("Could not find a asset with this key"));

	CScript scriptPubKeyFromOrig;
	if (!strAddressFrom.empty()) {
		scriptPubKeyFromOrig = GetScriptForDestination(addressFrom);
	}
    
	CAmount amount = AssetAmountFromValueNonNeg(params[2], theAsset.nPrecision);
	string ethAddress = params[3].get_str();
    boost::erase_all(ethAddress, "0x");  // strip 0x if exist
	theAssetAllocation.SetNull();
    theAssetAllocation.assetAllocationTuple.nAsset = std::move(assetAllocationTuple.nAsset);
    theAssetAllocation.assetAllocationTuple.witnessAddress = std::move(assetAllocationTuple.witnessAddress);    

	theAssetAllocation.listSendingAllocationAmounts.push_back(make_pair(CWitnessAddress(0, vchFromString("burn")), amount));

    vector<unsigned char> data;
    theAssetAllocation.Serialize(data);
    
    // convert to hex string because otherwise cscript will push a cscriptnum which is 4 bytes but we want 8 byte hex representation of an int64 pushed
    const std::string amountHex = int_to_hex(amount);
    
    const std::string assetHex = int_to_hex(nAsset);


	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ASSET_ALLOCATION) << CScript::EncodeOP_N(OP_ASSET_ALLOCATION_BURN) << ParseHex(assetHex) << ParseHex(amountHex) << theAsset.vchContract << ParseHex(ethAddress) << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyFromOrig;
	// send the asset pay txn
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CScript scriptData;
	scriptData << OP_RETURN << CScript::EncodeOP_N(OP_SYSCOIN_ASSET_ALLOCATION) << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, fee);
	vecSend.push_back(fee);

	return syscointxfund_helper("", vecSend);
}
UniValue assetallocationmint(const JSONRPCRequest& request) {
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    const UniValue &params = request.params;
    if (request.fHelp || 9 != params.size())
        throw runtime_error(
            "assetallocationmint [asset] [address] [amount] [blocknumber] [tx_hex] [txroot_hex] [txmerkleproof_hex] [txmerkleroofpath_hex] [witness]\n"
            "<asset> Asset guid.\n"
            "<address> Address that will get this minting.\n"
            "<amount> Amount of asset to mint. Note that fees will be taken from the owner address.\n"
            "<blocknumber> Block number of the block that included the burn transaction on Ethereum.\n"
            "<tx_hex> Raw transaction hex of the burn transaction on Ethereum.\n"
            "<txroot_hex> The transaction merkle root that commits this transaction to the block header.\n"
            "<txmerkleproof_hex> The list of parent nodes of the Merkle Patricia Tree for SPV proof.\n"
            "<txmerkleroofpath_hex> The merkle path to walk through the tree to recreate the merkle root.\n"
            "<witness> Witness address that will sign for web-of-trust notarization of this transaction.\n"
            + HelpRequiringPassphrase(pwallet));

    const int &nAsset = params[0].get_int();
    string strAddress = params[1].get_str();
    CAmount nAmount = AmountFromValue(params[2]);
    uint32_t nBlockNumber = (uint32_t)params[3].get_int();
    string vchTxRoot = params[4].get_str();
    string vchValue = params[5].get_str();
    string vchParentNodes = params[6].get_str();
    string vchPath = params[7].get_str();
    string strWitness = params[8].get_str();
    
    const CTxDestination &dest = DecodeDestination(strAddress);
    UniValue detail = DescribeAddress(dest);
    if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
        throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
    string witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str();
    unsigned char witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();
    vector<CRecipient> vecSend;
    
    CMintSyscoin mintSyscoin;
    mintSyscoin.assetAllocationTuple = CAssetAllocationTuple(nAsset, CWitnessAddress(witnessVersion, ParseHex(witnessProgramHex)));
    mintSyscoin.nValueAsset = nAmount;
    mintSyscoin.vchTxRoot = ParseHex(vchTxRoot);
    mintSyscoin.vchValue = ParseHex(vchValue);
    mintSyscoin.nBlockNumber = nBlockNumber;
    mintSyscoin.vchParentNodes = ParseHex(vchParentNodes);
    mintSyscoin.vchPath = ParseHex(vchPath);
    
    vector<unsigned char> data;
    mintSyscoin.Serialize(data);
    
    CScript scriptData;
    scriptData << OP_RETURN << CScript::EncodeOP_N(OP_SYSCOIN_MINT) << data;
    CRecipient fee;
    CreateFeeRecipient(scriptData, fee);
    vecSend.push_back(fee);
       
    return syscointxfund_helper(strWitness, vecSend, SYSCOIN_TX_VERSION_MINT_ASSET);
}
UniValue assetallocationsend(const JSONRPCRequest& request) {
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
	const UniValue &params = request.params;
	if (request.fHelp || params.size() != 4)
		throw runtime_error(
			"assetallocationsend [asset] [addressfrom] ([{\"address\":\"string\",\"amount\":amount},...] [witness]\n"
			"Send an asset allocation you own to another address. Maximimum recipients is 250.\n"
			"<asset> Asset guid.\n"
			"<addressfrom> Address that owns this asset allocation.\n"
			"<address> Address to transfer to.\n"
			"<amount> Quantity of asset to send.\n"
			"<witness> Witness address that will sign for web-of-trust notarization of this transaction.\n"
			+ HelpRequiringPassphrase(pwallet));

	// gather & validate inputs
	const int &nAsset = params[0].get_int();
	string vchAddressFrom = params[1].get_str();
	UniValue valueTo = params[2];
	vector<unsigned char> vchWitness;
    string strWitness = params[3].get_str();
	if (!valueTo.isArray())
		throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Array of receivers not found");
	string strAddressFrom;
	const string &strAddress = vchAddressFrom;
    CTxDestination addressFrom;
    string witnessProgramHex;
    unsigned char witnessVersion = 0;
    if(strAddress != "burn"){
	    addressFrom = DecodeDestination(strAddress);
    	if (IsValidDestination(addressFrom)) {
    		strAddressFrom = strAddress;
    	
            UniValue detail = DescribeAddress(addressFrom);
            if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
                throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
            witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str(); 
            witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();    
        }  
    }
    
	CAssetAllocation theAssetAllocation;
	const CAssetAllocationTuple assetAllocationTuple(nAsset, CWitnessAddress(witnessVersion, strAddress == "burn"? vchFromString("burn"): ParseHex(witnessProgramHex)));
	if (!GetAssetAllocation(assetAllocationTuple, theAssetAllocation))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1500 - " + _("Could not find a asset allocation with this key"));

	CAsset theAsset;
	if (!GetAsset(nAsset, theAsset))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1501 - " + _("Could not find a asset with this key"));

	theAssetAllocation.SetNull();
    theAssetAllocation.assetAllocationTuple.nAsset = std::move(assetAllocationTuple.nAsset);
    theAssetAllocation.assetAllocationTuple.witnessAddress = std::move(assetAllocationTuple.witnessAddress); 
	UniValue receivers = valueTo.get_array();
	
	for (unsigned int idx = 0; idx < receivers.size(); idx++) {
		const UniValue& receiver = receivers[idx];
		if (!receiver.isObject())
			throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"address'\" or \"amount\"}");

		const UniValue &receiverObj = receiver.get_obj();
		const string &toStr = find_value(receiverObj, "address").get_str();
        const CTxDestination &dest = DecodeDestination(toStr);
		if (!IsValidDestination(dest))
			throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1502 - " + _("Asset must be sent to a valid syscoin address"));
      
        UniValue detail = DescribeAddress(dest);
        if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
            throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
        string witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str();
        unsigned char witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();
        	
		UniValue amountObj = find_value(receiverObj, "amount");
		if (amountObj.isNum()) {
			const CAmount &amount = AssetAmountFromValue(amountObj, theAsset.nPrecision);
			if (amount <= 0)
				throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "amount must be positive");
			theAssetAllocation.listSendingAllocationAmounts.push_back(make_pair(CWitnessAddress(witnessVersion,ParseHex(witnessProgramHex)), amount));
		}
		else
			throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected amount as number in receiver array");

	}

	CScript scriptPubKeyFromOrig;
	if (!strAddressFrom.empty()) {
		scriptPubKeyFromOrig = GetScriptForDestination(addressFrom);
	}
    
	CScript scriptPubKey;
    {
        LOCK(cs_assetallocationarrival);
    	// check to see if a transaction for this asset/address tuple has arrived before minimum latency period
    	const ArrivalTimesMap &arrivalTimes = arrivalTimesMap[assetAllocationTuple.ToString()];
    	const int64_t & nNow = GetTimeMillis();
    	int minLatency = ZDAG_MINIMUM_LATENCY_SECONDS * 1000;
    	if (fUnitTest)
    		minLatency = 1000;
    	for (auto& arrivalTime : arrivalTimes) {
    		// if this tx arrived within the minimum latency period flag it as potentially conflicting
    		if ((nNow - arrivalTime.second) < minLatency) {
    			throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1503 - " + _("Please wait a few more seconds and try again..."));
    		}
    	}
    }

	vector<unsigned char> data;
	theAssetAllocation.Serialize(data);   

	scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ASSET_ALLOCATION) << CScript::EncodeOP_N(OP_ASSET_ALLOCATION_SEND) << OP_2DROP;
	scriptPubKey += scriptPubKeyFromOrig;
	// send the asset pay txn
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);	
	
	CScript scriptData;
	scriptData << OP_RETURN << CScript::EncodeOP_N(OP_SYSCOIN_ASSET_ALLOCATION) << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, fee);
	vecSend.push_back(fee);

	return syscointxfund_helper(strWitness, vecSend);
}


UniValue assetallocationinfo(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
    if (request.fHelp || 2 != params.size())
        throw runtime_error("assetallocationinfo <asset> <address>\n"
                "Show stored values of a single asset allocation.\n");

    const int &nAsset = params[0].get_int();
	string strAddressFrom = params[1].get_str();
    string witnessProgramHex = "";
    unsigned char witnessVersion = 0;
    if(strAddressFrom != "burn"){
        const CTxDestination &dest = DecodeDestination(strAddressFrom);
        UniValue detail = DescribeAddress(dest);
        if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
            throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
        witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str();
        witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();
    }
	UniValue oAssetAllocation(UniValue::VOBJ);
	const CAssetAllocationTuple assetAllocationTuple(nAsset, CWitnessAddress(witnessVersion, strAddressFrom == "burn"? vchFromString("burn"): ParseHex(witnessProgramHex)));
	CAssetAllocation txPos;
	if (passetallocationdb == nullptr || !passetallocationdb->ReadAssetAllocation(assetAllocationTuple, txPos))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1507 - " + _("Failed to read from assetallocation DB"));

	CAsset theAsset;
	if (!GetAsset(nAsset, theAsset))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1508 - " + _("Could not find a asset with this key"));


	if(!BuildAssetAllocationJson(txPos, theAsset, oAssetAllocation))
		oAssetAllocation.clear();
    return oAssetAllocation;
}
int DetectPotentialAssetAllocationSenderConflicts(const CAssetAllocationTuple& assetAllocationTupleSender, const uint256& lookForTxHash) {
    LOCK(cs_assetallocationarrival);
	CAssetAllocation dbAssetAllocation;
	// get last POW asset allocation balance to ensure we use POW balance to check for potential conflicts in mempool (real-time balances).
	// The idea is that real-time spending amounts can in some cases overrun the POW balance safely whereas in some cases some of the spends are 
	// put in another block due to not using enough fees or for other reasons that miners don't mine them.
	// We just want to flag them as level 1 so it warrants deeper investigation on receiver side if desired (if fund amounts being transferred are not negligible)
	if (passetallocationdb == nullptr || !passetallocationdb->ReadAssetAllocation(assetAllocationTupleSender, dbAssetAllocation))
		return ZDAG_NOT_FOUND;
        

	// ensure that this transaction exists in the arrivalTimes DB (which is the running stored lists of all real-time asset allocation sends not in POW)
	// the arrivalTimes DB is only added to for valid asset allocation sends that happen in real-time and it is removed once there is POW on that transaction
    const ArrivalTimesMap& arrivalTimes = arrivalTimesMap[assetAllocationTupleSender.ToString()];
	if(arrivalTimes.empty())
		return ZDAG_NOT_FOUND;
	// sort the arrivalTimesMap ascending based on arrival time value

	// Declaring the type of Predicate for comparing arrivalTimesMap
	typedef std::function<bool(std::pair<uint256, int64_t>, std::pair<uint256, int64_t>)> Comparator;

	// Defining a lambda function to compare two pairs. It will compare two pairs using second field
	Comparator compFunctor =
		[](std::pair<uint256, int64_t> elem1, std::pair<uint256, int64_t> elem2)
	{
		return elem1.second < elem2.second;
	};

	// Declaring a set that will store the pairs using above comparision logic
	std::set<std::pair<uint256, int64_t>, Comparator> arrivalTimesSet(
		arrivalTimes.begin(), arrivalTimes.end(), compFunctor);

	// go through arrival times and check that balances don't overrun the POW balance
	pair<uint256, int64_t> lastArrivalTime;
	lastArrivalTime.second = GetTimeMillis();
	unordered_map<string, CAmount> mapBalances;
	// init sender balance, track balances by address
	// this is important because asset allocations can be sent/received within blocks and will overrun balances prematurely if not tracked properly, for example pow balance 3, sender sends 3, gets 2 sends 2 (total send 3+2=5 > balance of 3 from last stored state, this is a valid scenario and shouldn't be flagged)
	CAmount &senderBalance = mapBalances[assetAllocationTupleSender.witnessAddress.ToString()];
	senderBalance = dbAssetAllocation.nBalance;
	int minLatency = ZDAG_MINIMUM_LATENCY_SECONDS * 1000;
	if (fUnitTest)
		minLatency = 1000;
	for (auto& arrivalTime : arrivalTimesSet)
	{
		// ensure mempool has this transaction and it is not yet mined, get the transaction in question
		const CTransactionRef txRef = mempool.get(arrivalTime.first);
		if (!txRef)
			continue;
		const CTransaction &tx = *txRef;

		// if this tx arrived within the minimum latency period flag it as potentially conflicting
		if (abs(arrivalTime.second - lastArrivalTime.second) < minLatency) {
			return ZDAG_MINOR_CONFLICT;
		}
		const uint256& txHash = tx.GetHash();
		CAssetAllocation assetallocation(tx);


		if (!assetallocation.listSendingAllocationAmounts.empty()) {
			for (auto& amountTuple : assetallocation.listSendingAllocationAmounts) {
				senderBalance -= amountTuple.second;
				mapBalances[amountTuple.first.ToString()] += amountTuple.second;
				// if running balance overruns the stored balance then we have a potential conflict
				if (senderBalance < 0) {
					return ZDAG_MINOR_CONFLICT;
				}
			}
		}
		// even if the sender may be flagged, the order of events suggests that this receiver should get his money confirmed upon pow because real-time balance is sufficient for this receiver
		if (txHash == lookForTxHash) {
			return ZDAG_STATUS_OK;
		}
	}
	return lookForTxHash.IsNull()? ZDAG_STATUS_OK: ZDAG_NOT_FOUND;
}
UniValue assetallocationsenderstatus(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 3 != params.size())
		throw runtime_error("assetallocationsenderstatus <asset> <address> <txid>\n"
			"Show status as it pertains to any current Z-DAG conflicts or warnings related to a sender or sender/txid combination of an asset allocation transfer. Leave txid empty if you are not checking for a specific transfer.\n"
			"Return value is in the status field and can represent 3 levels(0, 1 or 2)\n"
			"Level -1 means not found, not a ZDAG transaction, perhaps it is already confirmed.\n"
			"Level 0 means OK.\n"
			"Level 1 means warning (checked that in the mempool there are more spending balances than current POW sender balance). An active stance should be taken and perhaps a deeper analysis as to potential conflicts related to the sender.\n"
			"Level 2 means an active double spend was found and any depending asset allocation sends are also flagged as dangerous and should wait for POW confirmation before proceeding.\n");

	const int &nAsset = params[0].get_int();
	string strAddressSender = params[1].get_str();

    
    const CTxDestination &dest = DecodeDestination(strAddressSender);
    UniValue detail = DescribeAddress(dest);
    if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
        throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
    string witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str();
    unsigned char witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();
	uint256 txid;
	txid.SetNull();
	if(!params[2].get_str().empty())
		txid.SetHex(params[2].get_str());
	UniValue oAssetAllocationStatus(UniValue::VOBJ);
	const CAssetAllocationTuple assetAllocationTupleSender(nAsset, CWitnessAddress(witnessVersion, ParseHex(witnessProgramHex)));
    {
        LOCK2(cs_main, mempool.cs);
        ResetAssetAllocation(assetAllocationTupleSender.ToString(), txid, false, true);
        
    	int nStatus = ZDAG_STATUS_OK;
    	if (assetAllocationConflicts.find(assetAllocationTupleSender.ToString()) != assetAllocationConflicts.end())
    		nStatus = ZDAG_MAJOR_CONFLICT;
    	else {
    		nStatus = DetectPotentialAssetAllocationSenderConflicts(assetAllocationTupleSender, txid);
    	}
        oAssetAllocationStatus.pushKV("status", nStatus);
    }
	return oAssetAllocationStatus;
}
bool BuildAssetAllocationJson(const CAssetAllocation& assetallocation, const CAsset& asset, UniValue& oAssetAllocation)
{
    CAmount nBalanceZDAG = assetallocation.nBalance;
    const string &allocationTupleStr = assetallocation.assetAllocationTuple.ToString();
    {
        LOCK(cs_assetallocation);
        AssetBalanceMap::iterator mapIt =  mempoolMapAssetBalances.find(allocationTupleStr);
        if(mapIt != mempoolMapAssetBalances.end())
            nBalanceZDAG = mapIt->second;
    }
    oAssetAllocation.pushKV("_id", allocationTupleStr);
	oAssetAllocation.pushKV("asset", (int)assetallocation.assetAllocationTuple.nAsset);
	oAssetAllocation.pushKV("address",  assetallocation.assetAllocationTuple.witnessAddress.ToString());
	oAssetAllocation.pushKV("balance", ValueFromAssetAmount(assetallocation.nBalance, asset.nPrecision));
    oAssetAllocation.pushKV("balance_zdag", ValueFromAssetAmount(nBalanceZDAG, asset.nPrecision));
	return true;
}

void AssetAllocationTxToJSON(const int &op, const CTransaction &tx, UniValue &entry)
{
    CAssetAllocation assetallocation(tx);
    if(assetallocation.assetAllocationTuple.IsNull())
        return;
    CAsset dbAsset;
    GetAsset(assetallocation.assetAllocationTuple.nAsset, dbAsset);
    int nHeight = 0;
    uint256 hash_block;
    CBlockIndex* blockindex = nullptr;
    CTransactionRef txRef;
    if (GetTransaction(tx.GetHash(), txRef, Params().GetConsensus(), hash_block, true, blockindex) && blockindex)
        nHeight = blockindex->nHeight; 
    const string& strSender = assetallocation.assetAllocationTuple.witnessAddress.ToString();
    entry.pushKV("txtype", assetAllocationFromOp(op));
    entry.pushKV("_id", assetallocation.assetAllocationTuple.ToString());
    entry.pushKV("txid", tx.GetHash().GetHex());
    entry.pushKV("height", nHeight);
    entry.pushKV("asset", (int)assetallocation.assetAllocationTuple.nAsset);
    entry.pushKV("sender", strSender);
    UniValue oAssetAllocationReceiversArray(UniValue::VARR);
    CAmount nTotal = 0;
    string strCat = "";
    isminefilter filter = ISMINE_SPENDABLE;
    CWallet* const pwallet = GetDefaultWallet();
    if (fAssetAllocationIndex && strCat.empty()) {
        if(!strSender.empty() && strSender != "burn" && pwallet)
        {
            isminefilter mine = IsMine(*pwallet, DecodeDestination(strSender));
            if ((mine & filter)) {
                strCat = "send";
            }
        }
    }   
    const bool& bSending = strCat == "send";
    if (!assetallocation.listSendingAllocationAmounts.empty()) {
        for (auto& amountTuple : assetallocation.listSendingAllocationAmounts) {
            nTotal += amountTuple.second;
            const string& strReceiver = amountTuple.first.ToString();
            UniValue oAssetAllocationReceiversObj(UniValue::VOBJ);
            oAssetAllocationReceiversObj.pushKV("address", strReceiver);
            oAssetAllocationReceiversObj.pushKV("amount", ValueFromAssetAmount(bSending? -amountTuple.second:amountTuple.second, dbAsset.nPrecision));
            oAssetAllocationReceiversArray.push_back(oAssetAllocationReceiversObj);
             if (fAssetAllocationIndex && strCat.empty()) {
                if(!strReceiver.empty() && strReceiver != "burn" && pwallet){
                    isminefilter mine = IsMine(*pwallet, DecodeDestination(strReceiver));
                    if ((mine & filter))
                        strCat = "receive";
                } 
            }           
        }
    }
    entry.pushKV("allocations", oAssetAllocationReceiversArray);
    entry.pushKV("total", ValueFromAssetAmount(bSending? -nTotal: nTotal, dbAsset.nPrecision));

    if(!strCat.empty())
        entry.pushKV("category", strCat);
    
}
bool AssetAllocationTxToJSON(const int &op, const CTransaction &tx, const CAsset& dbAsset, const int& nHeight, const bool& confirmed, UniValue &entry, string& strSender)
{
    CAssetAllocation assetallocation(tx);
    if(assetallocation.assetAllocationTuple.IsNull() || dbAsset.IsNull())
        return false;
    strSender = assetallocation.assetAllocationTuple.witnessAddress.ToString();
    entry.pushKV("txtype", assetAllocationFromOp(op));
    entry.pushKV("_id", assetallocation.assetAllocationTuple.ToString());
    entry.pushKV("txid", tx.GetHash().GetHex());
    entry.pushKV("height", nHeight);
    entry.pushKV("asset", (int)assetallocation.assetAllocationTuple.nAsset);
    entry.pushKV("sender", strSender);
    UniValue oAssetAllocationReceiversArray(UniValue::VARR);
    CAmount nTotal = 0;
    string strCat = "";
    isminefilter filter = ISMINE_SPENDABLE;
    CWallet* const pwallet = GetDefaultWallet();
    if (fAssetAllocationIndex && strCat.empty()) {
        if(!strSender.empty() && strSender != "burn" && pwallet)
        {
            isminefilter mine = IsMine(*pwallet, DecodeDestination(strSender));
            if ((mine & filter)) {
                strCat = "send";
            }
        }
    }   
    const bool& bSending = strCat == "send";
    if (!assetallocation.listSendingAllocationAmounts.empty()) {
        for (auto& amountTuple : assetallocation.listSendingAllocationAmounts) {
            nTotal += amountTuple.second;
            const string& strReceiver = amountTuple.first.ToString();
            UniValue oAssetAllocationReceiversObj(UniValue::VOBJ);
            oAssetAllocationReceiversObj.pushKV("address", strReceiver);
            oAssetAllocationReceiversObj.pushKV("amount", ValueFromAssetAmount(bSending? -amountTuple.second:amountTuple.second, dbAsset.nPrecision));
            oAssetAllocationReceiversArray.push_back(oAssetAllocationReceiversObj);
             if (fAssetAllocationIndex && strCat.empty()) {
                if(!strReceiver.empty() && strReceiver != "burn" && pwallet){
                    isminefilter mine = IsMine(*pwallet, DecodeDestination(strReceiver));
                    if ((mine & filter))
                        strCat = "receive";
                } 
            }           
        }
    }
    entry.pushKV("allocations", oAssetAllocationReceiversArray);
    entry.pushKV("total", ValueFromAssetAmount(bSending? -nTotal: nTotal, dbAsset.nPrecision));

    if(!strCat.empty())
        entry.pushKV("category", strCat);
    entry.pushKV("confirmed", confirmed);   
    return !strCat.empty();

}
void AssetMintTxToJson(const CTransaction& tx, UniValue &entry){
    CMintSyscoin mintsyscoin(tx);
    if (!mintsyscoin.IsNull() && !mintsyscoin.assetAllocationTuple.IsNull()) {
        int nHeight = 0;
        uint256 hash_block;
        CTransactionRef txRef;
        CBlockIndex* blockindex = nullptr;
        if (GetTransaction(tx.GetHash(), txRef, Params().GetConsensus(), hash_block, true, blockindex) && blockindex)
            nHeight = blockindex->nHeight; 
        entry.pushKV("txtype", "assetallocationmint");
        entry.pushKV("_id", mintsyscoin.assetAllocationTuple.ToString());
        entry.pushKV("txid", tx.GetHash().GetHex());
        entry.pushKV("height", nHeight);
        entry.pushKV("asset", (int)mintsyscoin.assetAllocationTuple.nAsset);
        entry.pushKV("address", "");
        UniValue oAssetAllocationReceiversArray(UniValue::VARR);
        CAsset dbAsset;
        GetAsset(mintsyscoin.assetAllocationTuple.nAsset, dbAsset);
       
        UniValue oAssetAllocationReceiversObj(UniValue::VOBJ);
        oAssetAllocationReceiversObj.pushKV("address", mintsyscoin.assetAllocationTuple.witnessAddress.ToString());
        oAssetAllocationReceiversObj.pushKV("amount", ValueFromAssetAmount(mintsyscoin.nValueAsset, dbAsset.nPrecision));
        oAssetAllocationReceiversArray.push_back(oAssetAllocationReceiversObj);
    
        entry.pushKV("allocations", oAssetAllocationReceiversArray);                                        
    }                    
}
void AssetMintTxToJson(const CTransaction& tx, const CMintSyscoin& mintsyscoin, const int& nHeight, UniValue &entry){
    if (!mintsyscoin.IsNull() && !mintsyscoin.assetAllocationTuple.IsNull()) {
        entry.pushKV("txtype", "assetallocationmint");
        entry.pushKV("_id", mintsyscoin.assetAllocationTuple.ToString());
        entry.pushKV("txid", tx.GetHash().GetHex());
        entry.pushKV("height", nHeight);       
        entry.pushKV("asset", (int)mintsyscoin.assetAllocationTuple.nAsset);
        entry.pushKV("address", "");
        UniValue oAssetAllocationReceiversArray(UniValue::VARR);
        CAsset dbAsset;
        GetAsset(mintsyscoin.assetAllocationTuple.nAsset, dbAsset);
       
        UniValue oAssetAllocationReceiversObj(UniValue::VOBJ);
        oAssetAllocationReceiversObj.pushKV("address", mintsyscoin.assetAllocationTuple.witnessAddress.ToString());
        oAssetAllocationReceiversObj.pushKV("amount", ValueFromAssetAmount(mintsyscoin.nValueAsset, dbAsset.nPrecision));
        oAssetAllocationReceiversArray.push_back(oAssetAllocationReceiversObj);
    
        entry.pushKV("allocations", oAssetAllocationReceiversArray);                                        
    }                    
}
std::vector<std::string> vecIntersection(std::vector<std::string> &v1,
                                      std::vector<std::string> &v2){
    std::vector<std::string> v3;

    std::sort(v1.begin(), v1.end());
    std::sort(v2.begin(), v2.end());

    std::set_intersection(v1.begin(),v1.end(),
                          v2.begin(),v2.end(),
                          back_inserter(v3));
    return v3;
}
bool CAssetAllocationTransactionsDB::ScanAssetAllocationIndex(const int count, const int from, const UniValue& oOptions, UniValue& oRes) {
	string strTxid = "";
	vector<string> vecSenders;
	vector<string> vecReceivers;
	string strAsset = "";
	bool bParseKey = false;
	if (!oOptions.isNull()) {
		const UniValue &txid = find_value(oOptions, "txid");
		if (txid.isStr()) {
			strTxid = txid.get_str();
			bParseKey = true;
		}
		const UniValue &asset = find_value(oOptions, "asset");
		if (asset.isStr()) {
			strAsset = asset.get_str();
			bParseKey = true;
		}

		const UniValue &owners = find_value(oOptions, "receivers");
		if (owners.isArray()) {
			const UniValue &ownersArray = owners.get_array();
			for (unsigned int i = 0; i < ownersArray.size(); i++) {
				const UniValue &owner = ownersArray[i].get_obj();
				const UniValue &ownerStr = find_value(owner, "address");
				if (ownerStr.isStr()) {
					bParseKey = true;
					vecReceivers.push_back(ownerStr.get_str());
				}
			}
		}

		const UniValue &senders = find_value(oOptions, "senders");
		if (senders.isArray()) {
			const UniValue &sendersArray = senders.get_array();
			for (unsigned int i = 0; i < sendersArray.size(); i++) {
				const UniValue &sender = sendersArray[i].get_obj();
				const UniValue &senderStr = find_value(sender, "address");
				if (senderStr.isStr()) {
					bParseKey = true;
					vecSenders.push_back(senderStr.get_str());
				}
			}
		}
	}
	int index = 0;
	UniValue assetValue;
	vector<string> contents;
	contents.reserve(5);
    {
        LOCK(cs_assetallocationindex);
    	for (auto&indexObj : boost::adaptors::reverse(AssetAllocationIndex)) {
    		for (auto& indexItem : indexObj.second) {
    			if (bParseKey) {
    				boost::algorithm::split(contents, indexItem.first, boost::is_any_of("-"));
    				if (!strTxid.empty() && strTxid != contents[0])
    					continue;
    				if (!strAsset.empty() && strAsset != contents[1])
    					continue;
    				if (!vecSenders.empty() && std::find(vecSenders.begin(), vecSenders.end(), contents[2]) == vecSenders.end())
    					continue;
    			}
    			index += 1;
    			if (index <= from) {
    				continue;
    			}
    			if (assetValue.read(indexItem.second)){
                    if(!vecReceivers.empty()){
                        vector<string> vecAddresses;
                        const UniValue &allocationsArray = find_value(assetValue, "allocations").get_array();
                        for(unsigned int i =0;i<allocationsArray.size();i++){
                            vecAddresses.push_back(find_value(allocationsArray[i].get_obj(),  "address").get_str());
                        }
                        const vector<string> &res = vecIntersection(vecAddresses,vecReceivers);
                        if(res.empty())
                            continue;
                    }
    				oRes.push_back(assetValue);
                }
    			if (index >= count + from)
    				break;
    		}
    		if (index >= count + from)
    			break;
    	}
    }
	return true;
}
bool CAssetAllocationMempoolDB::ScanAssetAllocationMempoolBalances(const int count, const int from, const UniValue& oOptions, UniValue& oRes) {
    string strTxid = "";
    vector<string> vecSenders;
    vector<string> vecReceivers;
    string strAsset = "";
    if (!oOptions.isNull()) {
       
        const UniValue &senders = find_value(oOptions, "senders");
        if (senders.isArray()) {
            const UniValue &sendersArray = senders.get_array();
            for (unsigned int i = 0; i < sendersArray.size(); i++) {
                const UniValue &sender = sendersArray[i].get_obj();
                const UniValue &senderStr = find_value(sender, "address");
                if (senderStr.isStr()) {
                    vecSenders.push_back(senderStr.get_str());
                }
            }
        }
    }
    int index = 0;
    {
        LOCK(cs_assetallocation);
        for (auto&indexObj : mempoolMapAssetBalances) {
            
            if (!vecSenders.empty() && std::find(vecSenders.begin(), vecSenders.end(), indexObj.first) == vecSenders.end())
                continue;
            index += 1;
            if (index <= from) {
                continue;
            }
            UniValue resultObj(UniValue::VOBJ);
            resultObj.pushKV(indexObj.first, ValueFromAmount(indexObj.second));
            oRes.push_back(resultObj);
            if (index >= count + from)
                break;
        }       
    }
    return true;
}
bool CAssetAllocationDB::Flush(const AssetAllocationMap &mapAssetAllocations){
    if(mapAssetAllocations.empty())
        return true;
    CDBBatch batch(*this);
    for (const auto &key : mapAssetAllocations) {
        if(key.second.IsNull()){
            batch.Erase(key.second.assetAllocationTuple);
        }
        else{
            batch.Write(key.second.assetAllocationTuple, key.second);
        }
    }
    LogPrint(BCLog::SYS, "Flushing %d asset allocations\n", mapAssetAllocations.size());
    return WriteBatch(batch);
}
bool CAssetAllocationDB::ScanAssetAllocations(const int count, const int from, const UniValue& oOptions, UniValue& oRes) {
	string strTxid = "";
	vector<CWitnessAddress> vecWitnessAddresses;
	uint32_t nAsset = 0;
	if (!oOptions.isNull()) {
		const UniValue &assetObj = find_value(oOptions, "asset");
		if(assetObj.isNum()) {
			nAsset = boost::lexical_cast<uint32_t>(assetObj.get_int());
		}

		const UniValue &owners = find_value(oOptions, "addresses");
		if (owners.isArray()) {
			const UniValue &ownersArray = owners.get_array();
			for (unsigned int i = 0; i < ownersArray.size(); i++) {
				const UniValue &owner = ownersArray[i].get_obj();
				const UniValue &ownerStr = find_value(owner, "address");
				if (ownerStr.isStr()) {
                    const CTxDestination &dest = DecodeDestination(ownerStr.get_str());
                    UniValue detail = DescribeAddress(dest);
                    if(find_value(detail.get_obj(), "iswitness").get_bool() == false)
                        throw runtime_error("SYSCOIN_ASSET_RPC_ERROR: ERRCODE: 2501 - " + _("Address must be a segwit based address"));
                    string witnessProgramHex = find_value(detail.get_obj(), "witness_program").get_str();
                    unsigned char witnessVersion = (unsigned char)find_value(detail.get_obj(), "witness_version").get_int();
					vecWitnessAddresses.push_back(CWitnessAddress(witnessVersion, ParseHex(witnessProgramHex)));
				}
			}
		}
	}

	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->SeekToFirst();
	CAssetAllocation txPos;
    CAssetAllocationTuple key;
	CAsset theAsset;
	int index = 0;
	while (pcursor->Valid()) {
		boost::this_thread::interruption_point();
		try {
			if (pcursor->GetKey(key) && (nAsset == 0 || nAsset != key.nAsset)) {
				pcursor->GetValue(txPos);              
				if (!vecWitnessAddresses.empty() && std::find(vecWitnessAddresses.begin(), vecWitnessAddresses.end(), txPos.assetAllocationTuple.witnessAddress) == vecWitnessAddresses.end())
				{
					pcursor->Next();
					continue;
				}
                if (!GetAsset(key.nAsset, theAsset))
                {
                    pcursor->Next();
                    continue;
                } 
				UniValue oAssetAllocation(UniValue::VOBJ);
				if (!BuildAssetAllocationJson(txPos, theAsset, oAssetAllocation)) 
				{
					pcursor->Next();
					continue;
				}
				index += 1;
				if (index <= from) {
					pcursor->Next();
					continue;
				}
				oRes.push_back(oAssetAllocation);
				if (index >= count + from) {
					break;
				}
			}
			pcursor->Next();
		}
		catch (std::exception &e) {
			return error("%s() : deserialize error", __PRETTY_FUNCTION__);
		}
	}
	return true;
}
UniValue listassetallocationtransactions(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 3 < params.size())
		throw runtime_error("listassetallocationtransactions [count] [from] [{options}]\n"
			"list asset allocations sent or recieved in this wallet. -assetallocationindex must be set as a startup parameter to use this function.\n"
			"[count]          (numeric, optional, default=10) The number of results to return, 0 to return all.\n"
			"[from]           (numeric, optional, default=0) The number of results to skip.\n"
			"[options]        (object, optional) A json object with options to filter results\n"
			"    {\n"
			"      \"txid\":txid					(string) Transaction ID to filter.\n"
			"	   \"asset\":guid					(number) Asset GUID to filter.\n"
			"	   \"senders\"						(array) a json array with senders\n"
			"		[\n"
			"			{\n"
			"				\"address\":string		(string) Sender address to filter.\n"
			"			} \n"
			"			,...\n"
			"		]\n"
			"	   \"receivers\"					(array) a json array with receivers\n"
			"		[\n"
			"			{\n"
			"				\"address\":string		(string) Receiver address to filter.\n"
			"			} \n"
			"			,...\n"
			"		]\n"
			"    }\n"
			+ HelpExampleCli("listassetallocationtransactions", "0 10")
			+ HelpExampleCli("listassetallocationtransactions", "0 0 '{\"asset\":343773}'")
			+ HelpExampleCli("listassetallocationtransactions", "0 0 '{\"senders\":[{\"address\":\"SfaMwYY19Dh96B9qQcJQuiNykVRTzXMsZR\"},{\"address\":\"SfaMwYY19Dh96B9qQcJQuiNykVRTzXMsZR\"}]}'")
			+ HelpExampleCli("listassetallocationtransactions", "0 0 '{\"txid\":\"1c7f966dab21119bac53213a2bc7532bff1fa844c124fd750a7d0b1332440bd1\"}'")
		);
	UniValue options;
	int count = 10;
	int from = 0;
	if (params.size() > 0)
		count = params[0].get_int();
	if (params.size() > 1)
		from = params[1].get_int();
	if (params.size() > 2)
		options = params[2];
	if (!fAssetAllocationIndex) {
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1509 - " + _("Asset allocation index not enabled, you must enable -assetallocationindex as a startup parameter or through syscoin.conf file to use this function.")); 
	}
	UniValue oRes(UniValue::VARR);
	if (!passetallocationtransactionsdb->ScanAssetAllocationIndex(count, from, options, oRes))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1509 - " + _("Scan failed"));
	return oRes;
}
UniValue listassetallocations(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 3 < params.size())
		throw runtime_error("listassetallocations [count] [from] [{options}]\n"
			"scan through all asset allocations.\n"
			"[count]          (numeric, optional, default=10) The number of results to return.\n"
			"[from]           (numeric, optional, default=0) The number of results to skip.\n"
			"[options]        (array, optional) A json object with options to filter results\n"
			"    {\n"
			"	   \"asset\":guid					(number) Asset GUID to filter.\n"
			"	   \"addresses\"					(array) a json array with addresses owning allocations\n"
			"		[\n"
			"			{\n"
			"				\"address\":string		(string) Address to filter.\n"
			"			} \n"
			"			,...\n"
			"		]\n"
			"    }\n"
			+ HelpExampleCli("listassetallocations", "0")
			+ HelpExampleCli("listassetallocations", "10 10")
			+ HelpExampleCli("listassetallocations", "0 0 '{\"asset\":92922}'")
			+ HelpExampleCli("listassetallocations", "0 0 '{\"addresses\":[{\"address\":\"SfaMwYY19Dh96B9qQcJQuiNykVRTzXMsZR\"},{\"address\":\"SfaMwYY19Dh96B9qQcJQuiNykVRTzXMsZR\"}]}'")
		);
	UniValue options;
	int count = 10;
	int from = 0;
	if (params.size() > 0) {
		count = params[0].get_int();
		if (count == 0) {
			count = 10;
		} else
		if (count < 0) {
			throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1510 - " + _("'count' must be 0 or greater"));
		}
	}
	if (params.size() > 1) {
		from = params[1].get_int();
		if (from < 0) {
			throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1510 - " + _("'from' must be 0 or greater"));
		}
	}
	if (params.size() > 2) {
		options = params[2];
	}
	UniValue oRes(UniValue::VARR);
	if (!passetallocationdb->ScanAssetAllocations(count, from, options, oRes))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1510 - " + _("Scan failed"));
	return oRes;
}
UniValue listassetallocationmempoolbalances(const JSONRPCRequest& request) {
    const UniValue &params = request.params;
    if (request.fHelp || 3 < params.size())
        throw runtime_error("listassetallocationmempoolbalances [count] [from] [{options}]\n"
            "scan through all asset allocations.\n"
            "[count]          (numeric, optional, default=10) The number of results to return.\n"
            "[from]           (numeric, optional, default=0) The number of results to skip.\n"
            "[options]        (array, optional) A json object with options to filter results\n"
            "    {\n"
            "      \"senders\"                    (array) a json array with senders\n"
            "       [\n"
            "           {\n"
            "               \"address\":string      (string) Sender address to filter.\n"
            "           } \n"
            "           ,...\n"
            "       ]\n"
            "    }\n"
            + HelpExampleCli("listassetallocations", "0")
            + HelpExampleCli("listassetallocations", "10 10")
            + HelpExampleCli("listassetallocations", "0 0 '{\"senders\":[{\"address\":\"SfaMwYY19Dh96B9qQcJQuiNykVRTzXMsZR\"},{\"address\":\"SfaMwYY19Dh96B9qQcJQuiNykVRTzXMsZR\"}]}'")
        );
    UniValue options;
    int count = 10;
    int from = 0;
    if (params.size() > 0) {
        count = params[0].get_int();
        if (count == 0) {
            count = 10;
        } else
        if (count < 0) {
            throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1510 - " + _("'count' must be 0 or greater"));
        }
    }
    if (params.size() > 1) {
        from = params[1].get_int();
        if (from < 0) {
            throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1510 - " + _("'from' must be 0 or greater"));
        }
    }
    if (params.size() > 2) {
        options = params[2];
    }
    UniValue oRes(UniValue::VARR);
    if (!passetallocationmempooldb->ScanAssetAllocationMempoolBalances(count, from, options, oRes))
        throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 1510 - " + _("Scan failed"));
    return oRes;
}