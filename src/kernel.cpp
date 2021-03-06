// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The Gridcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>
#include "kernel.h"
#include "txdb.h"

std::string YesNo(bool bin);
double GetPoSKernelPS2();
std::string RoundToString(double d, int place);
bool IsCPIDValidv2(MiningCPID& mc,int height);
using namespace std;
MiningCPID DeserializeBoincBlock(std::string block);
std::string RetrieveMd5(std::string s1);
extern std::string CPIDByAddress(std::string address);
bool IsCPIDValid_Retired(std::string cpid, std::string ENCboincpubkey);
MiningCPID GetMiningCPID();
StructCPID GetStructCPID();
extern int64_t GetRSAWeightByCPID(std::string cpid);
extern double OwedByAddress(std::string address);
std::string ExtractXML(std::string XMLdata, std::string key, std::string key_end);
double cdbl(std::string s, int place);
extern int DetermineCPIDType(std::string cpid);
std::string GetHttpPage(std::string cpid, bool usedns, bool clearcache);
extern int64_t GetRSAWeightByCPIDWithRA(std::string cpid);
double MintLimiter(double PORDiff,int64_t RSA_WEIGHT,std::string cpid,int64_t locktime);
double GetBlockDifficulty(unsigned int nBits);
extern double GetLastPaymentTimeByCPID(std::string cpid);
extern double GetUntrustedMagnitude(std::string cpid, double& out_owed);
bool LessVerbose(int iMax1000);
StructCPID GetInitializedStructCPID2(std::string name,std::map<std::string, StructCPID>& vRef);
extern double ReturnTotalRacByCPID(std::string cpid);

typedef std::map<int, unsigned int> MapModifierCheckpoints;
/*
        ( 0, 0x0e00670bu )
        ( 1600, 0x1b3404a2 )
        ( 7000, 0x4da1176e )
*/

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
        ( 0, 0x0e00670bu )
    ;

// Hard checkpoints of stake modifiers to ensure they are deterministic (testNet)
static std::map<int, unsigned int> mapStakeModifierCheckpointsTestNet =
    boost::assign::map_list_of
        ( 0, 0x0e00670bu )
    ;

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
{
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low

    return min(nIntervalEnd - nIntervalBeginning - nStakeMinAge, (int64_t)nStakeMaxAge);
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    BOOST_FOREACH(const PAIRTYPE(int64_t, uint256)& item, vSortedByTimestamp)
    {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        CDataStream ss(SER_GETHASH, 0);
        ss << pindex->hashProof << nStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    if (fDebug && GetBoolArg("-printstakemodifier"))
        printf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");
    if (fDebug10)
    {
        printf("ComputeNextStakeModifier: prev modifier=0x%016"PRIx64" time=%s\n", nStakeModifier, DateTimeStrFormat(nModifierTime).c_str());
    }
    if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * nModifierInterval / GetTargetSpacing(pindexPrev->nHeight));
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (fDebug && GetBoolArg("-printstakemodifier"))
            printf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat(nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (fDebug && GetBoolArg("-printstakemodifier"))
    {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        BOOST_FOREACH(const PAIRTYPE(uint256, const CBlockIndex*)& item, mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake()? "S" : "W");
        }
        printf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate,
			pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: new modifier=0x%016"PRIx64" time=%s\n", nStakeModifierNew, DateTimeStrFormat(pindexPrev->GetBlockTime()).c_str());
    }

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    nStakeModifier = 0;
    if (!mapBlockIndex.count(hashBlockFrom))
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mapBlockIndex[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();
    const CBlockIndex* pindex = pindexFrom;
    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        if (!pindex->pnext)
        {   // reached best block; may happen if node is behind on block chain
            if (fPrintProofOfStake || (pindex->GetBlockTime() + nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    pindex->GetBlockHash().ToString().c_str(), pindex->nHeight, hashBlockFrom.ToString().c_str());
            else
                return false;
        }
        pindex = pindex->pnext;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// ppcoin kernel protocol
// cfoinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                  future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//

double ReturnTotalRacByCPID(std::string cpid)
{
	std::string result = GetHttpPage(cpid,true,true);
	//std::vector<std::string> vRAC = split(result.c_str(),"<project>");
	std::string sRAC = ExtractXML(result,"<expavg_credit>","</expavg_credit>");
	double dRAC = cdbl(sRAC,0);
	return dRAC;						
}

int DetermineCPIDType(std::string cpid)
{
	// -1 = Invalid CPID
	//  1 = Valid CPID with RAC
	//  2 = Investor or Pool Miner
	if (cpid.empty()) return -1;
	if (cpid=="INVESTOR") return 2;
	StructCPID h = mvMagnitudes[cpid];
	if (h.Magnitude > 0) return 1;
	// If magnitude is 0 in the current superblock, try to get magnitude from netsoft before failing
	double dRAC = ReturnTotalRacByCPID(cpid);
	if (dRAC > 0) return 1;
	// At this point, they have no RAC in the superblock, and no RAC in Netsoft, so we assume they are an investor (or pool miner)
	return 2;
}	


double GetMagnitudeByHashBoinc(std::string hashBoinc, int height)
{
	if (hashBoinc.length() > 1)
		{
			MiningCPID boincblock = DeserializeBoincBlock(hashBoinc);
			if (boincblock.cpid == "" || boincblock.cpid.length() < 6) return 0;  //Block has no CPID
			if (boincblock.cpid == "INVESTOR")  return 0;
   			if (boincblock.projectname == "") 	return 0;
    		if (boincblock.rac < 100) 			return 0;
			if (!IsCPIDValidv2(boincblock,height)) return 0;
			return boincblock.Magnitude;
		}
		return 0;
}


std::string CPIDByAddress(std::string address)
{
		   //CryptoLottery
		   for(map<string,StructCPID>::iterator ii=mvMagnitudes.begin(); ii!=mvMagnitudes.end(); ++ii)
		   {
				StructCPID structMag = GetStructCPID();
				structMag = mvMagnitudes[(*ii).first];
				if (structMag.initialized && structMag.cpid.length() > 2 && structMag.cpid != "INVESTOR" && structMag.GRCAddress.length() > 5)
				{
					if (structMag.GRCAddress==address)
					{
						return structMag.cpid;
					}
		     	}
			}
		   return "";

}



double OwedByAddress(std::string address)
{
		   double outstanding = 0;
		   for(map<string,StructCPID>::iterator ii=mvMagnitudes.begin(); ii!=mvMagnitudes.end(); ++ii)
		   {
				StructCPID structMag = GetStructCPID();
				structMag = mvMagnitudes[(*ii).first];
				if (structMag.initialized && structMag.cpid.length() > 2 && structMag.cpid != "INVESTOR" && structMag.GRCAddress.length() > 5)
				{
					if (structMag.GRCAddress==address)
					{
						double out1 = structMag.totalowed - structMag.payments;
						return out1;
					}
		     	}
			}
		   return outstanding;
}

int64_t GetRSAWeightByCPIDWithRA(std::string cpid)
{
	//Requires Magnitude > 0, be in superblock, with a lifetime cpid paid = 0
	//12-3-2015
	if (cpid.empty() || cpid=="INVESTOR" || cpid=="POOL") return 0;
	double dWeight = 0;
	StructCPID stMagnitude = GetInitializedStructCPID2(cpid,mvMagnitudes);	
	StructCPID stLifetime  = GetInitializedStructCPID2(cpid,mvResearchAge);
	if (stMagnitude.Magnitude > 0 && stLifetime.ResearchSubsidy == 0)
	{
		dWeight = 100000;
	}
	return dWeight;
}

int64_t GetRSAWeightByCPID(std::string cpid)
{

	if (IsResearchAgeEnabled(pindexBest->nHeight) && AreBinarySuperblocksEnabled(pindexBest->nHeight)) 
	{
			return GetRSAWeightByCPIDWithRA(cpid);
			//ToDo : During next mandatory, retire this old function.
	}


	double weight = 0;
	if (cpid=="" || cpid=="INVESTOR") return 5000;
	if (mvMagnitudes.size() > 0)
	{
			StructCPID UntrustedHost = GetStructCPID();
			UntrustedHost = mvMagnitudes[cpid]; //Contains Consensus Magnitude
			if (UntrustedHost.initialized)
			{
						double mag_accuracy = UntrustedHost.Accuracy;
						if (mag_accuracy >= 0 && mag_accuracy <= 5)
						{
							weight=25000;
						}
						else if (mag_accuracy > 5)
						{
								weight = (UntrustedHost.owed*14) + UntrustedHost.Magnitude;
								if (fTestNet && weight < 0) weight = 0;
								if (IsResearchAgeEnabled(pindexBest->nHeight) && weight < 0) weight=0;
						}

			}
			else
			{
				if (cpid.length() > 5 && cpid != "INVESTOR")
				{
						weight = 25000;
				}
			}
	}
	else
	{
		if (cpid.length() > 5 && cpid != "INVESTOR")
		{
				weight = 5000;
		}
	}
	if (fTestNet && weight > 25000) weight = 25000;
	int64_t RSA_WEIGHT = weight;
	return RSA_WEIGHT;
}


double GetLastPaymentTimeByCPID(std::string cpid)
{
	double lpt = 0;
	if (mvMagnitudes.size() > 0)
	{
			StructCPID UntrustedHost = GetStructCPID();
			UntrustedHost = mvMagnitudes[cpid]; //Contains Consensus Magnitude
			if (UntrustedHost.initialized)
			{
						double mag_accuracy = UntrustedHost.Accuracy;
						if (mag_accuracy > 0)
						{
								lpt = UntrustedHost.LastPaymentTime;
						}
			}
			else
			{
				if (cpid.length() > 5 && cpid != "INVESTOR")
				{
						lpt = 0;
				}
			}
	}
	else
	{
		if (cpid.length() > 5 && cpid != "INVESTOR")
		{
				lpt=0;
		}
	}
	return lpt;
}

int64_t GetRSAWeightByBlock(MiningCPID boincblock)
{
	int64_t rsa_weight = 0;
	if (boincblock.cpid != "INVESTOR")
	{
		rsa_weight = boincblock.RSAWeight + boincblock.Magnitude;
	}

	if (fTestNet && rsa_weight < 0) rsa_weight = 0;
	if (IsResearchAgeEnabled(pindexBest->nHeight) && rsa_weight < 0) rsa_weight = 0;
	return rsa_weight;
}

double GetUntrustedMagnitude(std::string cpid, double& out_owed)
{
	out_owed = 0;
	double mag = 0;
	if (mvMagnitudes.size() > 0)
	{
			StructCPID UntrustedHost = GetStructCPID();
			UntrustedHost = mvMagnitudes[cpid]; //Contains Consensus Magnitude
			if (UntrustedHost.initialized)
			{
						double mag_accuracy = UntrustedHost.Accuracy;
						if (mag_accuracy > 0) out_owed = UntrustedHost.owed;
						mag = UntrustedHost.Magnitude;
			}
			else
			{
				if (cpid.length() > 5 && cpid != "INVESTOR")
				{
						out_owed = 0;
				}
			}
	}
	else
	{
		if (cpid.length() > 5 && cpid != "INVESTOR")
		{
				out_owed = 0;
		}
	}
	return mag;
}



static bool CheckStakeKernelHashV1(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset,
	const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake,
	uint256& targetProofOfStake, bool fPrintProofOfStake, std::string hashBoinc, bool checking_local)
{

	//Note: When client is checking locally for a good kernelhash, block.vtx[0] is still null, and block.vtx[1].GetValueOut() is null (for mint) so hashBoinc is provided separately
	double PORDiff = GetBlockDifficulty(nBits);
    if (nTimeTx < txPrev.nTime)
	{
		// Transaction timestamp violation
		printf(".TimestampViolation.");
        return error("CheckStakeKernelHash() : nTime violation");
	}

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    if (nTimeBlockFrom + nStakeMinAge > nTimeTx)
	{
		// Min age requirement
		printf(".MinimumAgeRequirementViolation.,");
        return error("CheckStakeKernelHash() : min age violation");
	}

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    uint256 hashBlockFrom = blockFrom.GetHash();
	MiningCPID boincblock = DeserializeBoincBlock(hashBoinc);
	std::string cpid = boincblock.cpid;
    int64_t RSA_WEIGHT = 0;
	int oNC = 0;
	//12-7-2014 R Halford
	RSA_WEIGHT = GetRSAWeightByBlock(boincblock);

	CBigNum bnCoinDayWeight = 0;
	if (checking_local)
	{
		bnCoinDayWeight = CBigNum(nValueIn) * GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx) / COIN / (24*60*60);
	}
	else
	{
		bnCoinDayWeight = CBigNum(nValueIn) * GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx) / COIN / (24*60*60);
	}

	double coin_age = std::abs((double)nTimeTx-(double)txPrev.nTime);
	double payment_age = std::abs((double)nTimeTx-(double)boincblock.LastPaymentTime);
	if ((payment_age > 60*60) && boincblock.Magnitude > 1 && boincblock.cpid != "INVESTOR" && (coin_age > 4*60*60) && (coin_age > RSA_WEIGHT)
		&& (RSA_WEIGHT/14 > MintLimiter(PORDiff,RSA_WEIGHT,boincblock.cpid,nTimeTx)) )
	{
		//Coins are older than RSA balance
		oNC=1;
	}

	if (fDebug) if (LessVerbose(100)) printf("CPID %s, BlockTime %f PrevTxTime %f Payment_Age %f, Coin_Age %f, NC %f, RSA_WEIGHT %f, Magnitude %f; ",
		boincblock.cpid.c_str(), (double)blockFrom.GetBlockTime(),(double)txPrev.nTime,
		payment_age,coin_age,(double)oNC,(double)RSA_WEIGHT,(double)boincblock.Magnitude);

	if (RSA_WEIGHT > 0) if (!IsCPIDValidv2(boincblock,1))
	{
		printf("Check stake kernelhash: CPID Invalid: RSA Weight = 0");
		oNC=0;
		if (checking_local) msMiningErrors6="CPID_INVALID";
	}

    targetProofOfStake = (bnCoinDayWeight * bnTargetPerCoinDay).getuint256();

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;
	if (boincblock.cpid=="INVESTOR")
	{
		if (!GetKernelStakeModifier(hashBlockFrom, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake))
		{
			printf("`");
			return false;
		}
	}
    ss << nStakeModifier;

    ss << nTimeBlockFrom << nTxPrevOffset << txPrev.nTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());
    if (fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier 0x%016"PRIx64" at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            mapBlockIndex[hashBlockFrom]->nHeight,
            DateTimeStrFormat(blockFrom.GetBlockTime()).c_str());
            printf("CheckStakeKernelHash() : check modifier=0x%016"PRIx64" nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (CBigNum(hashProofOfStake) > (bnCoinDayWeight * bnTargetPerCoinDay))
	{
		if (oNC==0)
		{
			//Use this area to log the submitters cpid and mint amount:
			if (!checking_local || fDebug)
			{
				if (LessVerbose(75)) printf("{Vitals}: cpid %s, project %s, RSA_WEIGHT: %f \r\n",cpid.c_str(),boincblock.projectname.c_str(),(double)RSA_WEIGHT);
			}
			return false;
		}
	}
    if (fDebug && !fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier 0x%016"PRIx64" at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            mapBlockIndex[hashBlockFrom]->nHeight,
            DateTimeStrFormat(blockFrom.GetBlockTime()).c_str());
        printf("CheckStakeKernelHash() : pass modifier=0x%016"PRIx64" nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }
    return true;
}

// GridcoinCoin kernel protocol (credit goes to Black-Coin)
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//


static bool CheckStakeKernelHashV3(CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nTimeBlockFrom,
	const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake,
	uint256& targetProofOfStake, bool fPrintProofOfStake, std::string hashBoinc, bool checking_local, double por_nonce)
{

	double PORDiff = GetBlockDifficulty(nBits);
	MiningCPID boincblock = DeserializeBoincBlock(hashBoinc);
	double payment_age = std::abs((double)nTimeTx-(double)boincblock.LastPaymentTime);
    int64_t RSA_WEIGHT = GetRSAWeightByBlock(boincblock);
 	double coin_age = std::abs((double)nTimeTx-(double)txPrev.nTime);
	double BitsAge = PORDiff * 144;     //For every 100 Diff in Bits, two hours of coin age for researchers
	if (BitsAge > 86400) BitsAge=86400; //Limit to 24 hours (possibility of astronomical diff)
	// Halford : Explain to the Researcher why they are not staking:
	if (checking_local && LessVerbose(100))
	{
		std::string narr = "";
		if (boincblock.cpid != "INVESTOR")
		{
			if (boincblock.Magnitude < .25)   narr += "Magnitude too low for POR reward.";
			//if (payment_age < 60*60)          narr += "Recent: " + RoundToString(payment_age,0);
			if (payment_age < BitsAge)        narr += " Payment < Diff: " + RoundToString(payment_age,0) + "; " + RoundToString(BitsAge,0);
			if (coin_age < 4*60*60)           narr += " Coin Age (immature): " + RoundToString(coin_age,0);
			if (coin_age < RSA_WEIGHT)        narr += " Coin Age < RSA_Weight: " + RoundToString(coin_age,0) + " " + RoundToString(RSA_WEIGHT,0);
			if (RSA_WEIGHT/14 < MintLimiter(PORDiff,RSA_WEIGHT,boincblock.cpid,nTimeTx) && narr.empty())
			{
				narr += " RSAWeight<MintLimiter: "+ RoundToString(RSA_WEIGHT/14,0) + "; " + RoundToString(MintLimiter(PORDiff,RSA_WEIGHT,boincblock.cpid,nTimeTx),0);
			}
		}
		if (RSA_WEIGHT >= 24999 && boincblock.Magnitude > .25)        msMiningErrors7="Newbie block being generated.";
		msMiningErrors5 = narr;
	}


	if (fDebug && !checking_local)
	{
		printf("{CheckStakeKernelHashV3::INFO::} PaymentAge %f, BitsAge %f, Magnitude %f, Coin_Age %f, RSAWeight %f \r\n",
					(double)payment_age,(double)BitsAge,(double)boincblock.Magnitude,(double)coin_age,(double)RSA_WEIGHT);
	}

	if (boincblock.cpid != "INVESTOR")
	{
		if ((payment_age > 60*60) && (payment_age > BitsAge) && boincblock.Magnitude > 1 && (coin_age > 4*60*60) && (coin_age > RSA_WEIGHT))
		{
			//Coins are older than RSA balance - Allow hash to dictate outcome
		}
		else
		{
			return false;
		}
	}

    if (nTimeTx < txPrev.nTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    if (nTimeBlockFrom + nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");

    // Base target
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target 1-25-2015 Halford
    int64_t nValueIn = 0;
	nValueIn = txPrev.vout[prevout.n].nValue + (RSA_WEIGHT*COIN);
	CBigNum bnWeight = CBigNum(nValueIn/125000);
    bnTarget *= bnWeight;
    targetProofOfStake = bnTarget.getuint256();

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
	ss << RSA_WEIGHT << nTimeBlockFrom << txPrev.nTime << prevout.hash << prevout.n << nTimeTx <<  por_nonce;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    // Now check if proof-of-stake hash meets target protocol

	if (fDebug && !checking_local)
	{
				//Detailed Logging for Linux - Windows difference in calculation result
				printf("{CheckStakeKernelHashV3::INFO:::: RSA_WEIGHT %f, TimeBlockFrom %f, PrevnTime %f, PrevOutHash %s, PrevOut %f, nTimeTx %f, Por_Nonce %f \r\n",
					(double)RSA_WEIGHT,(double)nTimeBlockFrom, (double)txPrev.nTime, prevout.hash.GetHex().c_str(), (double)prevout.n, (double)nTimeTx, (double)por_nonce);
	}

    if (CBigNum(hashProofOfStake) > bnTarget)
	{
   		if (!checking_local)
		{
				//Detailed Logging for Linux - Windows difference in calculation result
				printf("{CheckStakeKernelHashV3::FailureInKernelHash}:: RSA_WEIGHT %f, TimeBlockFrom %f, PrevnTime %f, PrevOutHash %s, PrevOut %f, nTimeTx %f, Por_Nonce %f \r\n",
					(double)RSA_WEIGHT,(double)nTimeBlockFrom, (double)txPrev.nTime, prevout.hash.GetHex().c_str(), (double)prevout.n, (double)nTimeTx, (double)por_nonce);

		}
		return false;
	}
    return true;
}





bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset,
	const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake,
	uint256& targetProofOfStake, std::string hashBoinc, bool fPrintProofOfStake, bool checking_local, double por_nonce)
{
    if (IsProtocolV2(pindexPrev->nHeight+1))
        return CheckStakeKernelHashV3(pindexPrev, nBits, blockFrom.GetBlockTime(), txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake, hashBoinc, checking_local, por_nonce);
    else
		return CheckStakeKernelHashV1(nBits, blockFrom, nTxPrevOffset, txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake, hashBoinc, checking_local);
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits,
	uint256& hashProofOfStake, uint256& targetProofOfStake, std::string hashBoinc, bool checking_local, double por_nonce)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTxDB txdb("r");
    CTransaction txPrev;
    CTxIndex txindex;
    if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
        return tx.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, 0))
        return tx.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str()));

    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
        return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction


	printf("-");
	//if (pindexPrev) nGrandfatherHeight = pindexPrev->nHeight;

	if (pindexPrev->nHeight > nGrandfather)
	{
		if (!CheckStakeKernelHash(pindexPrev, nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, txPrev, txin.prevout, tx.nTime, hashProofOfStake,
			targetProofOfStake, hashBoinc, fDebug, checking_local, por_nonce))
		{
			uint256 diff1 = hashProofOfStake - targetProofOfStake;
			uint256 diff2 = targetProofOfStake - hashProofOfStake;
			std::string byme = checking_local ? "Generated_By_Me" : "Researcher";
			if (fDebug3) printf("CheckProofOfStake[%s] : INFO: check kernel failed on coinstake %s, Nonce %f, hashProof=%s, target=%s, offby1: %s, OffBy2: %s",
				byme.c_str(),
				tx.GetHash().ToString().c_str(), (double)por_nonce, hashProofOfStake.ToString().c_str(), targetProofOfStake.ToString().c_str(),
				diff1.ToString().c_str(), diff2.ToString().c_str());

			if (checking_local) return false;
			return false;
			//return error("CheckProofOfStake[%s] : INFO: check kernel failed on coinstake %s",byme.c_str(),tx.GetHash().ToString().c_str()));
			// may occur during initial download or if behind on block chain sync

		}
	}
    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int nHeight, int64_t nTimeBlock, int64_t nTimeTx)
{
    if (IsProtocolV2(nHeight))
        return (nTimeBlock == nTimeTx) && ((nTimeTx & STAKE_TIMESTAMP_MASK) == 0);
    else
        return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert (pindex->pprev || pindex->GetBlockHash() == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet));
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << (pindex->IsProofOfStake() ? pindex->hashProof : 0) << pindex->nStakeModifier;
    uint256 hashChecksum = Hash(ss.begin(), ss.end());
    hashChecksum >>= (256 - 32);
    return hashChecksum.Get64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
	return true;
}

