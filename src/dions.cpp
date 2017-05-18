// Copyright (c) 2017 IODigital foundation developers 
//
//

#include "db.h"
#include "txdb-leveldb.h"
#include "keystore.h"
#include "wallet.h"
#include "init.h"
#include "dions.h"

#include "bitcoinrpc.h"

#include "main.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <boost/xpressive/xpressive_dynamic.hpp>

using namespace std;
using namespace json_spirit;

extern Object JSONRPCError(int code, const string& message);
template<typename T> void ConvertTo(Value& value, bool fAllowNull=false);

std::map<vchType, uint256> mapMyMessages;
std::map<vchType, uint256> mapLocator;
std::map<vchType, set<uint256> > mapState;
std::set<vchType> setNewHashes;

#ifdef GUI
extern std::map<uint160, vchType> mapLocatorHashes;
#endif

extern uint256 SignatureHash(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType);

extern bool Solver(const CKeyStore& keystore, const CScript& scriptPubKey, uint256 hash, int nHashType, CScript& scriptSigRet, txnouttype& type);
extern bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, const CTransaction& txTo, unsigned int nIn, int nHashType);
extern Value sendtoaddress(const Array& params, bool fHelp);

bool getImportedPubKey(string senderAddress, string recipientAddress, vchType& recipientPubKeyVch, vchType& aesKeyBase64EncryptedVch);
bool getImportedPubKey(string recipientAddress, vchType& recipientPubKeyVch);
int checkAddress(string addr, CBitcoinAddress& a);



vchType vchFromValue(const Value& value)
{
  const std::string str = value.get_str();
  return vchFromString(str);
}


vchType vchFromString(const std::string& str)
{
  const unsigned char* strbeg;
  strbeg = reinterpret_cast<const unsigned char*>(str.c_str());
  return vchType(strbeg, strbeg + str.size());
}

string stringFromVch(const vector<unsigned char> &vch)
{
  string res;
  vector<unsigned char>::const_iterator vi = vch.begin();
  while(vi != vch.end()) {
    res +=(char)(*vi);
    vi++;
  }
  return res;
}
int GetExpirationDepth(int nHeight)
{
  return 210000;
}
int GetDisplayExpirationDepth()
{
    return 210000;
}
int GetTxPosHeight(AliasIndex& txPos)
{
    return txPos.nHeight;
}
int GetTxPosHeight(CDiskTxPos& txPos)
{
    CBlock block;
    if(!block.ReadFromDisk(txPos.nFile, txPos.nBlockPos, false))
        return 0;
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if(mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex =(*mi).second;
    if(!pindex || !pindex->IsInMainChain())
        return 0;
    return pindex->nHeight;
}
int
aliasHeight(vector<unsigned char> vchAlias)
{
  vector<AliasIndex> vtxPos;
  LocatorNodeDB ln1Db("r");
  if(ln1Db.lKey(vchAlias))
    {
      if(!ln1Db.lGet(vchAlias, vtxPos))
        return error("aliasHeight() : failed to read from alias DB");
      if(vtxPos.empty())
        return -1;

      AliasIndex& txPos = vtxPos.back();
      return GetTxPosHeight(txPos);
    }

  return -1;
}
CScript aliasStrip(const CScript& scriptIn)
{
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if(!aliasScript(scriptIn, op, vvch, pc))
        throw runtime_error("aliasStrip() : could not decode alias script");
    return CScript(pc, scriptIn.end());
}
bool IsLocator(const CTransaction& tx, const CTxOut& txout)
{
    const CScript& scriptPubKey = aliasStrip(txout.scriptPubKey);
    CScript scriptSig;
    txnouttype whichType;
    if(!Solver(*pwalletMain, scriptPubKey, 0, 0, scriptSig, whichType))
        return false;
    return true;
}
bool txPost(const vector<pair<CScript, int64_t> >& vecSend, const CWalletTx& wtxIn, int nTxOut, CWalletTx& wtxNew, CReserveKey& reservekey, int64_t& nFeeRet)
{
    int64_t nValue = 0;
    BOOST_FOREACH(const PAIRTYPE(CScript, int64_t)& s, vecSend)
    {
        if(nValue < 0)
            return false;
        nValue += s.second;
    }
    if(vecSend.empty() || nValue < 0)
        return false;

    wtxNew.pwallet = pwalletMain;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        CTxDB txdb("r");
        ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
        {
            nFeeRet = nTransactionFee;
            for(;;)
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64_t nTotalValue = nValue + nFeeRet;
                BOOST_FOREACH(const PAIRTYPE(CScript, int64_t)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                int64_t nWtxinCredit = wtxIn.vout[nTxOut].nValue;

                set<pair<const CWalletTx*, unsigned int> > setCoins;
                int64_t nValueIn = 0;
                if(nTotalValue - nWtxinCredit > 0)
                {
                    if(!pwalletMain->SelectCoins(nTotalValue - nWtxinCredit, wtxNew.nTime, setCoins, nValueIn))
                    {
        		LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
        		LEAVE_CRITICAL_SECTION(cs_main)
                        return false;
                    }
                }


                vector<pair<const CWalletTx*, unsigned int> >
                    vecCoins(setCoins.begin(), setCoins.end());

                vecCoins.insert(vecCoins.begin(), make_pair(&wtxIn, nTxOut));

                nValueIn += nWtxinCredit;

                int64_t nChange = nValueIn - nTotalValue;
                if(nChange >= CENT)
                {
                    CPubKey pubkey;
                    if(!reservekey.GetReservedKey(pubkey))
                    {
                      return false;
                    }

                    CScript scriptChange;
                    scriptChange.SetDestination(pubkey.GetID());

                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));
                }

                int nIn = 0;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    if(!SignSignature(*pwalletMain, *coin.first, wtxNew, nIn++))
                        return false;
                }

                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK);
                if(nBytes >= MAX_BLOCK_SIZE)
                    return false;

                int64_t nPayFee = nTransactionFee *(1 +(int64_t)nBytes / 1000);
                int64_t nMinFee = wtxNew.GetMinFee(1, GMF_SEND, nBytes);

                if(nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
        LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)
    return true;
}
string txRelay(const CScript& scriptPubKey, int64_t nValue, const CWalletTx& wtxIn, CWalletTx& wtxNew, bool fAskFee)
{
    int nTxOut = aliasOutIndex(wtxIn);
    CReserveKey reservekey(pwalletMain);
    int64_t nFeeRequired;
    vector< pair<CScript, int64_t> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    if(!txPost(vecSend, wtxIn, nTxOut, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if(nValue + nFeeRequired > pwalletMain->GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        return strError;
    }

#ifdef GUI
    if(fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired))
        return "ABORTED";
#endif

    if(!pwalletMain->CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}
bool GetValueOfTxPos(AliasIndex& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    vchValue = txPos.vValue;
    CTransaction tx;
    if(!tx.ReadFromDisk(txPos.txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    hash = tx.GetHash();
    return true;
}
bool GetValueOfTxPos(CDiskTxPos& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    CTransaction tx;
    if(!tx.ReadFromDisk(txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    if(!aliasTxValue(tx, vchValue))
        return error("GetValueOfTxPos() : could not decode value from tx");
    hash = tx.GetHash();
    return true;
}
bool aliasTx(LocatorNodeDB& aliasCacheDB, const vector<unsigned char> &vchAlias, CTransaction& tx)
{

    vector<AliasIndex> vtxPos;
    if(!aliasCacheDB.lGet(vchAlias, vtxPos) || vtxPos.empty())
        return false;

    AliasIndex& txPos = vtxPos.back();

    int nHeight = txPos.nHeight;
    if(nHeight + GetExpirationDepth(pindexBest->nHeight) < pindexBest->nHeight)
    {
        string alias = stringFromVch(vchAlias);
        return false;
    }

    if(!tx.ReadFromDisk(txPos.txPos))
        return error("aliasTx() : could not read tx from disk");
    return true;
}
bool aliasAddress(const CTransaction& tx, std::string& strAddress)
{
    int op;
    int nOut;
    vector<vector<unsigned char> > vvch;
    if(!aliasTx(tx, op, nOut, vvch))
        return false;
    const CTxOut& txout = tx.vout[nOut];
    const CScript& scriptPubKey = aliasStrip(txout.scriptPubKey);
    strAddress = scriptPubKey.GetBitcoinAddress();
    return true;
}
bool aliasAddress(const CDiskTxPos& txPos, std::string& strAddress)
{
    CTransaction tx;
    if(!tx.ReadFromDisk(txPos))
        return error("aliasAddress() : could not read tx from disk");

    return aliasAddress(tx, strAddress);
}

Value myRSAKeys(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "myRSAKeys\n"
                "list my rsa keys "
                );

  Array jsonAddressRSAList;
  BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
  {

    const CBitcoinAddress& a = item.first;
    const string& aliasStr = item.second;
    Object oAddressInfo;
    oAddressInfo.push_back(Pair("address", a.ToString()));

    string d = "";
    int r = atod(a.ToString(), d);

    if(r == 0)
      oAddressInfo.push_back(Pair("alias", d));
    else
      oAddressInfo.push_back(Pair("alias", "NONE"));

    CKeyID keyID;
    if(!a.GetKeyID(keyID))
      throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
    {
      return jsonAddressRSAList;
    }

    CPubKey pubKey = key.GetPubKey();

    string rsaPrivKey;
    bool found = pwalletMain->envCP0(pubKey, rsaPrivKey);
    if(found == false)
      continue;

    string aesPlainBase64;
    pwalletMain->GetAESMetadata(pubKey, aesPlainBase64);
    oAddressInfo.push_back(Pair("AES_256_plain", aesPlainBase64));

    jsonAddressRSAList.push_back(oAddressInfo);

  }

  return jsonAddressRSAList;
}
Value publicKeyExports(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "publicKeyExports [<alias>]\n"
                "list exported public keys "
                );

    vchType vchNodeLocator;
    if(params.size() == 1)
      vchNodeLocator = vchFromValue(params[0]);

    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    Array oRes;
    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
          {
            const CWalletTx& tx = item.second;

            vchType vchSender, vchRecipient, vchKey, vchAes, vchSig;
            int nOut;
            if(!tx.GetPublicKeyUpdate(nOut, vchSender, vchRecipient, vchKey, vchAes, vchSig))
              continue;

            if(!IsMinePost(tx))
              continue;

            const int nHeight = tx.GetHeightInMainChain();
            if(nHeight == -1)
              continue;
            assert(nHeight >= 0);

            Object aliasObj;
            aliasObj.push_back(Pair("exported_to", stringFromVch(vchSender)));
            aliasObj.push_back(Pair("key", stringFromVch(vchKey)));
            aliasObj.push_back(Pair("aes256_encrypted", stringFromVch(vchAes)));
            aliasObj.push_back(Pair("signature", stringFromVch(vchSig)));

            oRes.push_back(aliasObj);


        }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)


    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, aliasMapVchObj)
        oRes.push_back(item.second);

    return oRes;
}
Value publicKeys(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "publicKeys [<alias>]\n"
                );
    vchType vchNodeLocator;
    if(params.size() == 1)
      vchNodeLocator = vchFromValue(params[0]);

    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    Array oRes;
    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
          {
            const CWalletTx& tx = item.second;

            vchType vchSender, vchRecipient, vchKey, vchAes, vchSig;
            int nOut;
            if(!tx.GetPublicKeyUpdate(nOut, vchSender, vchRecipient, vchKey, vchAes, vchSig))
              continue;

            string keySenderAddr = stringFromVch(vchSender);
            CBitcoinAddress r(keySenderAddr);
            CKeyID keyID;
            r.GetKeyID(keyID);
            CKey key;
            bool imported=false;
            if(!pwalletMain->GetKey(keyID, key))
            {
              imported=true;
            }

            const int nHeight = tx.GetHeightInMainChain();
            if(nHeight == -1)
              continue;
            assert(nHeight >= 0);

            string recipient = stringFromVch(vchRecipient);

            Object aliasObj;
            aliasObj.push_back(Pair("sender", stringFromVch(vchSender)));
            aliasObj.push_back(Pair("recipient", recipient));
            if(imported)
              aliasObj.push_back(Pair("status", "imported"));
            else
              aliasObj.push_back(Pair("status", "exported"));

            aliasObj.push_back(Pair("confirmed", "false"));

            aliasObj.push_back(Pair("key", stringFromVch(vchKey)));
            aliasObj.push_back(Pair("aes256_encrypted", stringFromVch(vchAes)));
            aliasObj.push_back(Pair("signature", stringFromVch(vchSig)));
            oRes.push_back(aliasObj);


        }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    for(unsigned int i=0; i<oRes.size(); i++)
    {
       Object& o = oRes[i].get_obj();

       string s = o[0].value_.get_str();
       string r = o[1].value_.get_str();
       string status = o[2].value_.get_str();
       for(unsigned int j=i+1; j<oRes.size(); j++)
       {
         Object& o1 = oRes[j].get_obj();
         if(s == o1[1].value_.get_str() && r == o1[0].value_.get_str())
         {
           o[3].value_ = "true";
           o1[3].value_ = "true";
         }
       }
    }

    return oRes;
}

bool isMyAddress(string addrStr)
{
  CBitcoinAddress r(addrStr);
  if(!r.IsValid())
  {
    return false;
  }

  CKeyID keyID;
  if(!r.GetKeyID(keyID))
    return false;

  CKey key;
  if(!pwalletMain->GetKey(keyID, key))
    return false;

  return true;
}

Value decryptedMessageList(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "decryptedMessageList [<alias>]\n"
                );
    vchType vchNodeLocator;
    if(params.size() == 1)
      vchNodeLocator = vchFromValue(params[0]);

    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    Array oRes;
    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
        {
            const CWalletTx& tx = item.second;

            vchType vchSender, vchRecipient, vchEncryptedMessage, ivVch, vchSig;
            int nOut;
            if(!tx.GetEncryptedMessageUpdate(nOut, vchSender, vchRecipient, vchEncryptedMessage, ivVch, vchSig))
              continue;


            const int nHeight = tx.GetHeightInMainChain();

            string myAddr;
            string foreignAddr;
            if(isMyAddress(stringFromVch(vchSender)))
            {
              myAddr = stringFromVch(vchSender);
              foreignAddr = stringFromVch(vchRecipient);
            }
            else
            {
              myAddr = stringFromVch(vchRecipient);
              foreignAddr = stringFromVch(vchSender);
            }

            Object aliasObj;
            aliasObj.push_back(Pair("sender", stringFromVch(vchSender)));
            aliasObj.push_back(Pair("recipient", stringFromVch(vchRecipient)));
            aliasObj.push_back(Pair("encrypted_message", stringFromVch(vchEncryptedMessage)));
            string t = DateTimeStrFormat(tx.GetTxTime());
            aliasObj.push_back(Pair("time", t));



            string rsaPrivKey;
            string recipient = stringFromVch(vchRecipient);

            CBitcoinAddress r(myAddr);
            if(!r.IsValid())
            {
              continue;
              LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
              LEAVE_CRITICAL_SECTION(cs_main)
              throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
            }

            CKeyID keyID;
            if(!r.GetKeyID(keyID))
              throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
            CKey key;
            pwalletMain->GetKey(keyID, key);
            CPubKey pubKey = key.GetPubKey();

            string aesBase64Plain;
            vector<unsigned char> aesRawVector;
            if(pwalletMain->GetAESMetadata(pubKey, aesBase64Plain))
            {
              bool fInvalid = false;
              aesRawVector = DecodeBase64(aesBase64Plain.c_str(), &fInvalid);
            }
            else
            {
              vchType aesKeyBase64EncryptedVch;
              vchType pub_key = pubKey.Raw();
              if(getImportedPubKey(myAddr, foreignAddr, pub_key, aesKeyBase64EncryptedVch))
              {
                string aesKeyBase64Encrypted = stringFromVch(aesKeyBase64EncryptedVch);

                string privRSAKey;
                if(!pwalletMain->envCP0(pubKey, privRSAKey))
                  throw JSONRPCError(RPC_TYPE_ERROR, "Failed to retrieve private RSA key");

                string decryptedAESKeyBase64;
                DecryptMessage(privRSAKey, aesKeyBase64Encrypted, decryptedAESKeyBase64);
                bool fInvalid = false;
                aesRawVector = DecodeBase64(decryptedAESKeyBase64.c_str(), &fInvalid);
              }
              else
              {
                throw JSONRPCError(RPC_WALLET_ERROR, "No local symmetric key and no imported symmetric key found for recipient");
              }
            }

            string decrypted;
            string iv128Base64 = stringFromVch(ivVch);
            DecryptMessageAES(stringFromVch(vchEncryptedMessage),
                              decrypted,
                              aesRawVector,
                              iv128Base64);

            aliasObj.push_back(Pair("plain_text", decrypted));
            aliasObj.push_back(Pair("iv128Base64", stringFromVch(ivVch)));
            aliasObj.push_back(Pair("signature", stringFromVch(vchSig)));

            oRes.push_back(aliasObj);

            mapAliasVchInt[vchSender] = nHeight;
            aliasMapVchObj[vchSender] = aliasObj;
        }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)


    return oRes;
}
Value plainTextMessageList(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "plainTextMessageList [<alias>]\n"
                );

    vchType vchNodeLocator;
    if(params.size() == 1)
      vchNodeLocator = vchFromValue(params[0]);

    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    Array oRes;
    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
        {
            const CWalletTx& tx = item.second;





            vchType vchSender, vchRecipient, vchEncryptedMessage, ivVch, vchSig;
            int nOut;
            if(!tx.GetEncryptedMessageUpdate(nOut, vchSender, vchRecipient, vchEncryptedMessage, ivVch, vchSig))
              continue;

            Object aliasObj;
            aliasObj.push_back(Pair("sender", stringFromVch(vchSender)));
            aliasObj.push_back(Pair("recipient", stringFromVch(vchRecipient)));
            aliasObj.push_back(Pair("message", stringFromVch(vchEncryptedMessage)));
            aliasObj.push_back(Pair("iv128Base64", stringFromVch(ivVch)));
            aliasObj.push_back(Pair("signature", stringFromVch(vchSig)));
            oRes.push_back(aliasObj);
        }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    return oRes;
}
Value getNodeRecord(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "getNodeRecord [<node opt>]\n"
                );

    string k1;
    vchType vchNodeLocator;
    if(params.size() == 1)
    {
      k1 =(params[0]).get_str();
      vchNodeLocator = vchFromValue(params[0]);
    }


    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
          {
            const CWalletTx& tx = item.second;

            vchType vchAlias, vchValue;
            int nOut;
            int op__=-1;
        if(!tx.aliasSet(op__, nOut, vchAlias, vchValue))
          continue;

        Object aliasObj;


        const int nHeight = tx.GetHeightInMainChain();
        if(nHeight == -1)
          continue;
        assert(nHeight >= 0);

        string decrypted = "";
        string value = stringFromVch(vchValue);

        string strAddress = "";
        aliasAddress(tx, strAddress);
        if(op__ == OP_ALIAS_ENCRYPTED)
        {
          string rsaPrivKey;
          CBitcoinAddress r(strAddress);
          if(!r.IsValid())
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

          CKeyID keyID;
          if(!r.GetKeyID(keyID))
            throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

          CKey key;
          if(!pwalletMain->GetKey(keyID, key))
          {
            continue;
          }

          CPubKey pubKey = key.GetPubKey();
          if(pwalletMain->envCP0(pubKey, rsaPrivKey) == false)
          {
            continue;
          }

          DecryptMessage(rsaPrivKey, stringFromVch(vchAlias), decrypted);
          if(k1 != decrypted) 
          {
            continue;
          }

        if(mapAliasVchInt.find(vchFromString(decrypted)) != mapAliasVchInt.end() && mapAliasVchInt[vchFromString(decrypted)] > nHeight)
        {
          continue;
        }
          aliasObj.push_back(Pair("alias", decrypted));

          aliasObj.push_back(Pair("encrypted", "true"));
          mapAliasVchInt[vchFromString(decrypted)] = nHeight;
        }
        else
        {
        if(mapAliasVchInt.find(vchAlias) != mapAliasVchInt.end() && mapAliasVchInt[vchAlias] > nHeight)
        {
          continue;
        }

          if(k1 != stringFromVch(vchAlias)) continue;

          aliasObj.push_back(Pair("alias", stringFromVch(vchAlias)));
          aliasObj.push_back(Pair("encrypted", "false"));
          mapAliasVchInt[vchAlias] = nHeight;
        }

        aliasObj.push_back(Pair("value", value));

        if(!IsMinePost(tx))
          aliasObj.push_back(Pair("transferred", 1));
        aliasObj.push_back(Pair("address", strAddress));
        aliasObj.push_back(Pair("nHeigt", nHeight));


        CBitcoinAddress keyAddress(strAddress);
        CKeyID keyID;
        keyAddress.GetKeyID(keyID);
        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        vchType vchRand;

        const int expiresIn = nHeight + GetDisplayExpirationDepth() - pindexBest->nHeight;
        aliasObj.push_back(Pair("expires_in", expiresIn));
        if(expiresIn <= 0)
          aliasObj.push_back(Pair("expired", 1));

        if(mapState.count(vchAlias) && mapState[vchAlias].size())
        {
            aliasObj.push_back(Pair("status", "pending_update"));
        }

        if(decrypted != "")
        {
          vchType d1 = vchFromString(decrypted);
          if(mapState.count(d1) && mapState[d1].size())
          {
              aliasObj.push_back(Pair("status", "pending_update"));
          }

        }


        if(op__ != OP_ALIAS_ENCRYPTED)
          aliasMapVchObj[vchAlias] = aliasObj;
        else
          aliasMapVchObj[vchFromString(decrypted)] = aliasObj;

      }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)


    Array oRes;
    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, aliasMapVchObj)
        oRes.push_back(item.second);

    return oRes;
}

bool searchAliasEncrypted2(string alias, uint256& wtxInHash)
{
  std::transform(alias.begin(), alias.end(), alias.begin(), ::tolower);
  bool found=false;
  ENTER_CRITICAL_SECTION(cs_main)
  {
    ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
    {
      BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                    pwalletMain->mapWallet)
      {
        const CWalletTx& tx = item.second;

        vchType vchAlias, vchValue;
        int nOut;
        int op__=-1;
        if(!tx.aliasSet(op__, nOut, vchAlias, vchValue))
          continue;

        if(tx.IsSpent(nOut))
          continue;

        const int nHeight = tx.GetHeightInMainChain();
        if(nHeight == -1)
              continue;
        assert(nHeight >= 0);

        string strAddress = "";
        aliasAddress(tx, strAddress);
        string decrypted;
        if(op__ == OP_ALIAS_ENCRYPTED)
        {
          string rsaPrivKey;
          CBitcoinAddress r(strAddress);
          if(!r.IsValid())
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

          CKeyID keyID;
          if(!r.GetKeyID(keyID))
            throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

          CKey key;
          if(!pwalletMain->GetKey(keyID, key))
            throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

          CPubKey pubKey = key.GetPubKey();
          if(pwalletMain->envCP0(pubKey, rsaPrivKey) == false)
          {
            throw JSONRPCError(RPC_WALLET_ERROR, "error p0");
          }

          DecryptMessage(rsaPrivKey, stringFromVch(vchAlias), decrypted);
          std::transform(decrypted.begin(), decrypted.end(), decrypted.begin(), ::tolower);
          if(decrypted == alias)
          {
            found=true;
            wtxInHash=tx.GetHash();
            break;
          }
        }
      }
    }
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
  }
  LEAVE_CRITICAL_SECTION(cs_main)

  return found;
}

bool searchAliasEncrypted(string alias, uint256& wtxInHash)
{
  bool found=false;
  ENTER_CRITICAL_SECTION(cs_main)
  {
    ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
    {
      BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                    pwalletMain->mapWallet)
      {
        const CWalletTx& tx = item.second;

        vchType vchAlias, vchValue;
        int nOut;
        int op__=-1;
        if(!tx.aliasSet(op__, nOut, vchAlias, vchValue))
          continue;

        if(tx.IsSpent(nOut))
          continue;

        const int nHeight = tx.GetHeightInMainChain();
        if(nHeight == -1)
              continue;
        assert(nHeight >= 0);

        string strAddress = "";
        aliasAddress(tx, strAddress);
        string decrypted;
        if(op__ == OP_ALIAS_ENCRYPTED)
        {
          string rsaPrivKey;
          CBitcoinAddress r(strAddress);
          if(!r.IsValid())
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

          CKeyID keyID;
          if(!r.GetKeyID(keyID))
            throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

          CKey key;
          if(!pwalletMain->GetKey(keyID, key))
            throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

          CPubKey pubKey = key.GetPubKey();
          if(pwalletMain->envCP0(pubKey, rsaPrivKey) == false)
          {
            throw JSONRPCError(RPC_WALLET_ERROR, "error p0");
          }

          DecryptMessage(rsaPrivKey, stringFromVch(vchAlias), decrypted);
          if(decrypted == alias)
          {
            found=true;
            wtxInHash=tx.GetHash();
            break;
          }
        }
      }
    }
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
  }
  LEAVE_CRITICAL_SECTION(cs_main)

  return found;
}

Value aliasList__(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "aliasList__ [<alias>]\n"
                );

    vchType vchNodeLocator;
    if(params.size() == 1)
      vchNodeLocator = vchFromValue(params[0]);

    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
          {
            const CWalletTx& tx = item.second;

            vchType vchAlias, vchValue;
            int nOut;
            int op__=-1;
        if(!tx.aliasSet(op__, nOut, vchAlias, vchValue))
          continue;

        Object aliasObj;


        if(!vchNodeLocator.empty() && vchNodeLocator != vchAlias)
          continue;

        const int nHeight = tx.GetHeightInMainChain();
        if(nHeight == -1)
          continue;
        assert(nHeight >= 0);

        string decrypted = "";

        string strAddress = "";
        aliasAddress(tx, strAddress);
        if(op__ == OP_ALIAS_ENCRYPTED)
        {
          string rsaPrivKey;
          CBitcoinAddress r(strAddress);
          if(!r.IsValid())
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

          CKeyID keyID;
          if(!r.GetKeyID(keyID))
            throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

          CKey key;
          if(!pwalletMain->GetKey(keyID, key))
          {
            continue;
          }

          CPubKey pubKey = key.GetPubKey();
          if(pwalletMain->envCP0(pubKey, rsaPrivKey) == false)
          {
            continue;
          }

          DecryptMessage(rsaPrivKey, stringFromVch(vchAlias), decrypted);
        if(mapAliasVchInt.find(vchFromString(decrypted)) != mapAliasVchInt.end() && mapAliasVchInt[vchFromString(decrypted)] > nHeight)
        {
          continue;
        }
          aliasObj.push_back(Pair("alias", decrypted));

          aliasObj.push_back(Pair("encrypted", "true"));
          mapAliasVchInt[vchFromString(decrypted)] = nHeight;
        }
        else
        {
        if(mapAliasVchInt.find(vchAlias) != mapAliasVchInt.end() && mapAliasVchInt[vchAlias] > nHeight)
        {
          continue;
        }
          aliasObj.push_back(Pair("alias", stringFromVch(vchAlias)));
          aliasObj.push_back(Pair("encrypted", "false"));
          mapAliasVchInt[vchAlias] = nHeight;
        }


        if(!IsMinePost(tx))
          aliasObj.push_back(Pair("transferred", 1));
        aliasObj.push_back(Pair("address", strAddress));
        aliasObj.push_back(Pair("nHeigt", nHeight));


        CBitcoinAddress keyAddress(strAddress);
        CKeyID keyID;
        keyAddress.GetKeyID(keyID);
        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        vchType vchRand;

        const int expiresIn = nHeight + GetDisplayExpirationDepth() - pindexBest->nHeight;
        aliasObj.push_back(Pair("expires_in", expiresIn));
        if(expiresIn <= 0)
          aliasObj.push_back(Pair("expired", 1));

        if(mapState.count(vchAlias) && mapState[vchAlias].size())
        {
            aliasObj.push_back(Pair("status", "pending_update"));
        }

        if(decrypted != "")
        {
          vchType d1 = vchFromString(decrypted);
          if(mapState.count(d1) && mapState[d1].size())
          {
              aliasObj.push_back(Pair("status", "pending_update"));
          }

        }


        if(op__ != OP_ALIAS_ENCRYPTED)
          aliasMapVchObj[vchAlias] = aliasObj;
        else
          aliasMapVchObj[vchFromString(decrypted)] = aliasObj;

      }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)


    Array oRes;
    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, aliasMapVchObj)
        oRes.push_back(item.second);

    return oRes;
}

Value aliasList(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 1)
        throw runtime_error(
                "aliasList [<alias>]\n"
                );

    vchType vchNodeLocator;
    if(params.size() == 1)
      vchNodeLocator = vchFromValue(params[0]);

    std::map<vchType, int> mapAliasVchInt;
    std::map<vchType, Object> aliasMapVchObj;

    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
          {
            const CWalletTx& tx = item.second;

            vchType vchAlias, vchValue;
            int nOut;
            int op__=-1;
        if(!tx.aliasSet(op__, nOut, vchAlias, vchValue))
          continue;

        Object aliasObj;

        if(!vchNodeLocator.empty() && vchNodeLocator != vchAlias)
          continue;

        const int nHeight = tx.GetHeightInMainChain();
        if(nHeight == -1)
          continue;
        assert(nHeight >= 0);

        string decrypted = "";
        string value = stringFromVch(vchValue);

        string strAddress = "";
        aliasAddress(tx, strAddress);
        if(op__ == OP_ALIAS_ENCRYPTED)
        {
          string rsaPrivKey;
          CBitcoinAddress r(strAddress);
          if(!r.IsValid())
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

          CKeyID keyID;
          if(!r.GetKeyID(keyID))
            throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

          CKey key;
          if(!pwalletMain->GetKey(keyID, key))
          {
            continue;
          }

          CPubKey pubKey = key.GetPubKey();
          if(pwalletMain->envCP0(pubKey, rsaPrivKey) == false)
          {
            continue;
          }

          DecryptMessage(rsaPrivKey, stringFromVch(vchAlias), decrypted);
        if(mapAliasVchInt.find(vchFromString(decrypted)) != mapAliasVchInt.end() && mapAliasVchInt[vchFromString(decrypted)] > nHeight)
        {
          continue;
        }
          aliasObj.push_back(Pair("alias", decrypted));

          aliasObj.push_back(Pair("encrypted", "true"));
          mapAliasVchInt[vchFromString(decrypted)] = nHeight;
        }
        else
        {
        if(mapAliasVchInt.find(vchAlias) != mapAliasVchInt.end() && mapAliasVchInt[vchAlias] > nHeight)
        {
          continue;
        }
          aliasObj.push_back(Pair("alias", stringFromVch(vchAlias)));
          aliasObj.push_back(Pair("encrypted", "false"));
          mapAliasVchInt[vchAlias] = nHeight;
        }


        aliasObj.push_back(Pair("value", value));

        if(!IsMinePost(tx))
          aliasObj.push_back(Pair("transferred", 1));
        aliasObj.push_back(Pair("address", strAddress));
        aliasObj.push_back(Pair("nHeigt", nHeight));


        CBitcoinAddress keyAddress(strAddress);
        CKeyID keyID;
        keyAddress.GetKeyID(keyID);
        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        vchType vchRand;

        const int expiresIn = nHeight + GetDisplayExpirationDepth() - pindexBest->nHeight;
        aliasObj.push_back(Pair("expires_in", expiresIn));
        if(expiresIn <= 0)
          aliasObj.push_back(Pair("expired", 1));

        if(mapState.count(vchAlias) && mapState[vchAlias].size())
        {
            aliasObj.push_back(Pair("status", "pending_update"));
        }

        if(decrypted != "")
        {
          vchType d1 = vchFromString(decrypted);
          if(mapState.count(d1) && mapState[d1].size())
          {
              aliasObj.push_back(Pair("status", "pending_update"));
          }

        }


        if(op__ != OP_ALIAS_ENCRYPTED)
          aliasMapVchObj[vchAlias] = aliasObj;
        else
          aliasMapVchObj[vchFromString(decrypted)] = aliasObj;

      }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)


    Array oRes;
    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, aliasMapVchObj)
        oRes.push_back(item.second);

    return oRes;
}
Value nodeDebug(const Array& params, bool fHelp)
{
    if(fHelp)
        throw runtime_error(
            "nodeDebug\n"
            "Dump pending transactions id in the debug file.\n");

    printf("Pending:\n----------------------------\n");
    pair<vector<unsigned char>, set<uint256> > pairPending;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        BOOST_FOREACH(pairPending, mapState)
        {
            string alias = stringFromVch(pairPending.first);
            uint256 hash;
            BOOST_FOREACH(hash, pairPending.second)
            {
                if(!pwalletMain->mapWallet.count(hash))
                    printf("foreign ");
                printf("    %s\n", hash.GetHex().c_str());
            }
        }
    }
    LEAVE_CRITICAL_SECTION(cs_main)
    printf("----------------------------\n");
    return true;
}
Value nodeDebug1(const Array& params, bool fHelp)
{
    if(fHelp || params.size() < 1)
        throw runtime_error(
            "nodeDebug1 <alias>\n"
            "Dump alias blocks number and transactions id in the debug file.\n");

    vector<unsigned char> vchAlias = vchFromValue(params[0]);
    ENTER_CRITICAL_SECTION(cs_main)
    {

        vector<AliasIndex> vtxPos;
        LocatorNodeDB aliasCacheDB("r");
        if(!aliasCacheDB.lGet(vchAlias, vtxPos))
        {
            error("failed to read from alias DB");
            return false;
        }

        AliasIndex txPos;
        BOOST_FOREACH(txPos, vtxPos)
        {
            CTransaction tx;
            if(!tx.ReadFromDisk(txPos.txPos))
            {
                error("could not read txpos %s", txPos.txPos.ToString().c_str());
                continue;
            }
        }
    }
    LEAVE_CRITICAL_SECTION(cs_main)
    printf("-------------------------\n");
    return true;
}

Value updateEncryptedAlias(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 3)
        throw runtime_error(
                "updateEncryptedAlias <alias> <value> <address>"
                + HelpRequiringPassphrase());

    const vchType vchAlias = vchFromString(params[0].get_str());
    vchType vchValue = vchFromString(params[1].get_str());

    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        if(mapState.count(vchAlias) && mapState[vchAlias].size())
        {
            error("updateEncryptedAlias() : there are %lu pending operations on that alias, including %s",
                    mapState[vchAlias].size(),
                    mapState[vchAlias].begin()->GetHex().c_str());
    LEAVE_CRITICAL_SECTION(cs_main)
            throw runtime_error("there are pending operations on that alias");
        }
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    string ownerAddrStr;
    {
        LocatorNodeDB aliasCacheDB("r");
        CTransaction tx;
        if(aliasTx(aliasCacheDB, vchAlias, tx))
        {
            error("updateEncryptedAlias() : this alias is already active with tx %s",
                    tx.GetHash().GetHex().c_str());
            throw runtime_error("this alias is already active");
        }
    }



    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

        uint256 wtxInHash;
        if(!searchAliasEncrypted(stringFromVch(vchAlias), wtxInHash))
        {
    LEAVE_CRITICAL_SECTION(cs_main)
          throw runtime_error("could not find a coin with this alias, try specifying the registerAlias transaction id");
        }


        if(!pwalletMain->mapWallet.count(wtxInHash))
        {
    LEAVE_CRITICAL_SECTION(cs_main)
            throw runtime_error("previous transaction is not in the wallet");
        }

        CScript scriptPubKeyOrig;

        CScript scriptPubKey;


        CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
        bool found = false;
        BOOST_FOREACH(CTxOut& out, wtxIn.vout)
        {
            vector<vector<unsigned char> > vvch;
            int op;
            if(aliasScript(out.scriptPubKey, op, vvch)) {
                if(op != OP_ALIAS_ENCRYPTED)
                  throw runtime_error("previous transaction was not an OP_ALIAS_ENCRYPTED");

              string encrypted = stringFromVch(vvch[0]);
              uint160 hash = uint160(vvch[3]);
              string value = stringFromVch(vchValue);

              CDataStream ss(SER_GETHASH, 0);
              ss << encrypted;
              ss << hash.ToString();
              ss << value;
              ss << string("0");

              CScript script;
              script.SetBitcoinAddress(stringFromVch(vvch[2]));

              CBitcoinAddress ownerAddr = script.GetBitcoinAddress();
              if(!ownerAddr.IsValid())
                throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

              CKeyID keyID;
              if(!ownerAddr.GetKeyID(keyID))
                throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");


              CKey key;
              if(!pwalletMain->GetKey(keyID, key))
                throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

              CPubKey vchPubKey;
              pwalletMain->GetPubKey(keyID, vchPubKey);

              vector<unsigned char> vchSig;
              if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
                  throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

              string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

              scriptPubKey << OP_ALIAS_ENCRYPTED << vvch[0] << vchFromString(sigBase64) << vvch[2] << vvch[3] << vchValue << vchFromString("0") << OP_2DROP << OP_2DROP << OP_2DROP << OP_DROP;
              scriptPubKeyOrig.SetBitcoinAddress(stringFromVch(vvch[2]));
              scriptPubKey += scriptPubKeyOrig;
              found = true;
            }
        }

        if(!found)
        {
            throw runtime_error("previous tx on this alias is not a alias tx");
        }

        string strError = txRelay(scriptPubKey, MIN_AMOUNT, wtxIn, wtx, false);
        if(strError != "")
        {
    LEAVE_CRITICAL_SECTION(cs_main)
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    return wtx.GetHash().GetHex();
}

Value decryptAlias(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 2 )
        throw runtime_error(
                "decryptAlias <alias> <address specified by owner>\n"
                + HelpRequiringPassphrase());

    string locatorStr = params[0].get_str();
    std::transform(locatorStr.begin(), locatorStr.end(), locatorStr.begin(), ::tolower);
    const vchType vchAlias = vchFromValue(locatorStr);
    const std::string addressOfOwner = params[1].get_str();

    CBitcoinAddress ownerAddr(addressOfOwner);
    if(!ownerAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

    CKeyID keyID;
    if(!ownerAddr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CPubKey vchPubKey;
    pwalletMain->GetPubKey(keyID, vchPubKey);

    string rsaPubKeyStr = "";
    if(!pwalletMain->envCP1(key.GetPubKey(), rsaPubKeyStr))
        throw JSONRPCError(RPC_WALLET_ERROR, "no rsa key available for address");

    vchType vchRand;
    string r_;
    if(!pwalletMain->GetRandomKeyMetadata(key.GetPubKey(), vchRand, r_))
        throw JSONRPCError(RPC_WALLET_ERROR, "no random key available for address");


    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        if(mapState.count(vchAlias) && mapState[vchAlias].size())
        {
            error("decryptAlias() : there are %lu pending operations on that alias, including %s",
                    mapState[vchAlias].size(),
                    mapState[vchAlias].begin()->GetHex().c_str());
    LEAVE_CRITICAL_SECTION(cs_main)
            throw runtime_error("there are pending operations on that alias");
        }
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    {
        LocatorNodeDB aliasCacheDB("r");
        CTransaction tx;
        if(aliasTx(aliasCacheDB, vchAlias, tx))
        {
            error("decryptAlias() : this alias is already active with tx %s",
                    tx.GetHash().GetHex().c_str());
            throw runtime_error("this alias is already active");
        }
    }


    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

        uint256 wtxInHash;
        if(!searchAliasEncrypted(stringFromVch(vchAlias), wtxInHash))
        {
    LEAVE_CRITICAL_SECTION(cs_main)
          throw runtime_error("could not find a coin with this alias, try specifying the registerAlias transaction id");
        }


        if(!pwalletMain->mapWallet.count(wtxInHash))
        {
    LEAVE_CRITICAL_SECTION(cs_main)
            throw runtime_error("previous transaction is not in the wallet");
        }

        CScript scriptPubKeyOrig;
        scriptPubKeyOrig.SetBitcoinAddress(addressOfOwner);

        CScript scriptPubKey;


        CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
        vector<unsigned char> vchPrevSig;
        bool found = false;
        BOOST_FOREACH(CTxOut& out, wtxIn.vout)
        {
            vector<vector<unsigned char> > vvch;
            int op;
            if(aliasScript(out.scriptPubKey, op, vvch)) {
                if(op != OP_ALIAS_ENCRYPTED)
                    throw runtime_error("previous transaction wasn't a registerAlias");
                CDataStream ss(SER_GETHASH, 0);
                ss << locatorStr;
                CScript script;
                script.SetBitcoinAddress(stringFromVch(vvch[2]));

                CBitcoinAddress ownerAddr = script.GetBitcoinAddress();
                if(!ownerAddr.IsValid())
                  throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

                CKeyID keyID;
                if(!ownerAddr.GetKeyID(keyID))
                  throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");


                CKey key;
                if(!pwalletMain->GetKey(keyID, key))
                  throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

                CPubKey vchPubKey;
                pwalletMain->GetPubKey(keyID, vchPubKey);

                vector<unsigned char> vchSig;
                if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
                  throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

                string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());


                scriptPubKey << OP_ALIAS_SET << vchAlias << vchFromString(sigBase64) << vchFromString(addressOfOwner) << vchRand << vvch[4] << OP_2DROP << OP_2DROP << OP_2DROP;
                scriptPubKey += scriptPubKeyOrig;

                found = true;
            }
        }

        if(!found)
        {
            throw runtime_error("previous tx on this alias is not a alias tx");
        }

        string strError = txRelay(scriptPubKey, MIN_AMOUNT, wtxIn, wtx, false);
        if(strError != "")
        {
    LEAVE_CRITICAL_SECTION(cs_main)
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    return wtx.GetHash().GetHex();
}

Value transferEncryptedAlias(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 3)
        throw runtime_error(
          "transferAlias <alias> <localaddress> <toaddress> transfer a alias to a new address"
          + HelpRequiringPassphrase());

    vchType vchAlias = vchFromValue(params[0]);
    const vchType vchLocal = vchFromValue(params[1]);

    CBitcoinAddress localAddr((params[1]).get_str());

    if(!localAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

    CKeyID localkeyID;
    if(!localAddr.GetKeyID(localkeyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");

    CKey localkey;
    if(!pwalletMain->GetKey(localkeyID, localkey))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    vchType vchRand;
    string r_;
    if(!pwalletMain->GetRandomKeyMetadata(localkey.GetPubKey(), vchRand, r_))
        throw JSONRPCError(RPC_WALLET_ERROR, "no random key available for address");

    const string recipientAddrStr=(params[2]).get_str();

    CBitcoinAddress recipientAddr(recipientAddrStr);
    if(!recipientAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

    vchType rVch = vchFromString(recipientAddrStr);

    CKeyID rkeyID;
    if(!recipientAddr.GetKeyID(rkeyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");

    CPubKey vchRecipientPubKey;
    if(!pwalletMain->GetPubKey(rkeyID, vchRecipientPubKey))
      throw JSONRPCError(RPC_TYPE_ERROR, "no recipient pub key");

    vchType v1 = vchRecipientPubKey.Raw();
    string v1Str = stringFromVch(v1);
      

    CBitcoinAddress tmp;
    tmp.Set(rkeyID);
    string a=(tmp).ToString();


    string locatorStr = stringFromVch(vchAlias);

    vchType recipientPubKeyVch;

    if(!getImportedPubKey(recipientAddrStr, recipientPubKeyVch))
    {
      throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "transferEncryptedAlias no RSA key for recipient");
    }

    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;
    CScript scriptPubKeyOrig;

    scriptPubKeyOrig.SetBitcoinAddress(recipientAddrStr);

    CScript scriptPubKey;

    ENTER_CRITICAL_SECTION(cs_main)
    {
      uint256 wtxInHash;
      if(!searchAliasEncrypted(stringFromVch(vchAlias), wtxInHash))
      {
        LEAVE_CRITICAL_SECTION(cs_main)
        throw runtime_error("could not find a coin with this alias, try specifying the registerAlias transaction id");
      }

      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
          if(mapState.count(vchAlias) && mapState[vchAlias].size())
          {
              error("updateEncryptedAlias() : there are %lu pending operations on that alias, including %s",
                      mapState[vchAlias].size(),
                      mapState[vchAlias].begin()->GetHex().c_str());
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("there are pending operations on that alias");
          }

          EnsureWalletIsUnlocked();

          if(!pwalletMain->mapWallet.count(wtxInHash))
          {
              error("updateEncryptedAlias() : this coin is not in your wallet %s",
                      wtxInHash.GetHex().c_str());
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("this coin is not in your wallet");
          }

          string encryptedAliasForRecipient;
          EncryptMessage(stringFromVch(recipientPubKeyVch), locatorStr, encryptedAliasForRecipient);

          string randBase64 = EncodeBase64(&vchRand[0], vchRand.size());
          string encryptedRandForRecipient;
          EncryptMessage(stringFromVch(recipientPubKeyVch), randBase64, encryptedRandForRecipient);

          const CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
        bool found = false;
        BOOST_FOREACH(const CTxOut& out, wtxIn.vout)
        {
            vector<vector<unsigned char> > vvch;
            int op;
            if(aliasScript(out.scriptPubKey, op, vvch)) {
                if(op != OP_ALIAS_ENCRYPTED)
                  throw runtime_error("previous transaction was not an OP_ALIAS_ENCRYPTED");

                 uint160 hash = uint160(vvch[3]);

                 CDataStream ss(SER_GETHASH, 0);
                 ss << encryptedAliasForRecipient;
                 ss << hash.ToString();
                 ss << stringFromVch(vvch[4]);
                 ss << encryptedRandForRecipient;

                CScript script;
                script.SetBitcoinAddress(stringFromVch(vvch[2]));

                CBitcoinAddress ownerAddr = script.GetBitcoinAddress();
                if(!ownerAddr.IsValid())
                  throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

                CKeyID keyID;
                if(!ownerAddr.GetKeyID(keyID))
                  throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");

                CKey key;
                if(!pwalletMain->GetKey(keyID, key))
                  throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

                vector<unsigned char> vchSig;
                if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
                  throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

                string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());
              scriptPubKey << OP_ALIAS_ENCRYPTED << vchFromString(encryptedAliasForRecipient) << vchFromString(sigBase64) << rVch << vvch[3] << vvch[4] << vchFromString(encryptedRandForRecipient) << OP_2DROP << OP_2DROP << OP_2DROP << OP_DROP;


              scriptPubKey += scriptPubKeyOrig;
              found = true;
              break;
            }
        }

          if(!found)
          {
            throw runtime_error("previous tx on this alias is not a alias tx");
          }

          string strError = txRelay(scriptPubKey, MIN_AMOUNT, wtxIn, wtx, false);

          if(strError != "")
          {
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
         }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)
    return wtx.GetHash().GetHex();
}
Value transferAlias(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 2)
        throw runtime_error(
          "transferAlias <alias> <toaddress> transfer a alias to a new address"
          + HelpRequiringPassphrase());

    vchType vchAlias = vchFromValue(params[0]);
    const vchType vchAddress = vchFromValue(params[1]);

    string locatorStr = stringFromVch(vchAlias);
    string addressStr = stringFromVch(vchAddress);


    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;
    CScript scriptPubKeyOrig;

    string strAddress = params[1].get_str();
    uint160 hash160;
    bool isValid = AddressToHash160(strAddress, hash160);
    if(!isValid)
      throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid dions address");
    scriptPubKeyOrig.SetBitcoinAddress(strAddress);

    CScript scriptPubKey;

    scriptPubKey << OP_ALIAS_RELAY << vchAlias ;


    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
          if(mapState.count(vchAlias) && mapState[vchAlias].size())
          {
              error("updateEncryptedAlias() : there are %lu pending operations on that alias, including %s",
                      mapState[vchAlias].size(),
                      mapState[vchAlias].begin()->GetHex().c_str());
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("there are pending operations on that alias");
          }

          EnsureWalletIsUnlocked();

          LocatorNodeDB aliasCacheDB("r");
          CTransaction tx;
          if(!aliasTx(aliasCacheDB, vchAlias, tx))
          {
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("could not find a coin with this alias");
          }


          uint256 wtxInHash = tx.GetHash();

          if(!pwalletMain->mapWallet.count(wtxInHash))
          {
              error("updateEncryptedAlias() : this coin is not in your wallet %s",
                      wtxInHash.GetHex().c_str());
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("this coin is not in your wallet");
          }

          const CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
          int op__;
          int nOut;
          vchType vchValue;
          wtxIn.aliasSet(op__, nOut, vchAlias, vchValue);

          scriptPubKey << vchValue << OP_2DROP << OP_DROP;
          scriptPubKey += scriptPubKeyOrig;

          string locatorStr = stringFromVch(vchAlias);
          string dataStr = stringFromVch(vchValue);
          string strError = txRelay(scriptPubKey, MIN_AMOUNT, wtxIn, wtx, false);
          if(strError != "")
          {
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
         }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)
    return wtx.GetHash().GetHex();
}

Value updateAlias(const Array& params, bool fHelp)
{
    if(fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
                "updateAlias <alias> <value> [<toaddress>]\nUpdate and possibly transfer a alias"
                + HelpRequiringPassphrase());
    string locatorStr = params[0].get_str();
    std::transform(locatorStr.begin(), locatorStr.end(), locatorStr.begin(), ::tolower);
    const vchType vchAlias = vchFromValue(locatorStr);
    const vchType vchValue = vchFromValue(params[1]);

    string valueStr = stringFromVch(vchValue);


    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;
    CScript scriptPubKeyOrig;

    if(params.size() == 3)
    {
        string strAddress = params[2].get_str();
        uint160 hash160;
        bool isValid = AddressToHash160(strAddress, hash160);
        if(!isValid)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid dions address");
        scriptPubKeyOrig.SetBitcoinAddress(strAddress);
    }
    else
    {
        vector<unsigned char> vchPubKey = pwalletMain->GetKeyFromKeyPool();
        scriptPubKeyOrig.SetBitcoinAddress(vchPubKey);
    }

    CScript scriptPubKey;
    scriptPubKey << OP_ALIAS_RELAY << vchAlias << vchValue << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
      ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
      {
          if(mapState.count(vchAlias) && mapState[vchAlias].size())
          {
              error("updateAlias() : there are %lu pending operations on that alias, including %s",
                      mapState[vchAlias].size(),
                      mapState[vchAlias].begin()->GetHex().c_str());
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("there are pending operations on that alias");
          }

          EnsureWalletIsUnlocked();

          LocatorNodeDB aliasCacheDB("r");
          CTransaction tx;
          if(!aliasTx(aliasCacheDB, vchAlias, tx))
          {
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("could not find a coin with this alias");
          }

          uint256 wtxInHash = tx.GetHash();

          if(!pwalletMain->mapWallet.count(wtxInHash))
          {
              error("updateAlias() : this coin is not in your wallet %s",
                      wtxInHash.GetHex().c_str());
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
              throw runtime_error("this coin is not in your wallet");
          }

          CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
          string strError = txRelay(scriptPubKey, MIN_AMOUNT, wtxIn, wtx, false);
          if(strError != "")
          {
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    LEAVE_CRITICAL_SECTION(cs_main)
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
         }
      }
      LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
    }
    LEAVE_CRITICAL_SECTION(cs_main)
    return wtx.GetHash().GetHex();
}


Value newPlublicKey(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 1)
        throw runtime_error(
                "newPlublicKey <public_address>"
                + HelpRequiringPassphrase());

  EnsureWalletIsUnlocked();

  CWalletDB walletdb(pwalletMain->strWalletFile, "r+");

  string myAddress = params[0].get_str();

  CBitcoinAddress addr(myAddress);
  if(!addr.IsValid())
    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

  CKeyID keyID;
  if(!addr.GetKeyID(keyID))
    throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

  CKey key;
  if(!pwalletMain->GetKey(keyID, key))
    throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

  CPubKey pubKey = key.GetPubKey();

  string testKey;

  if(!pwalletMain->SetRSAMetadata(pubKey))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to load meta data for key");

  if(!walletdb.UpdateKey(pubKey, pwalletMain->mapKeyMetadata[pubKey.GetID()]))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to write meta data for key");

  if(!pwalletMain->envCP1(pubKey, testKey))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to load meta data for key");

  string pKey;
  if(!pwalletMain->envCP0(pubKey, pKey))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to load meta data for key");

  vector<Value> res;
  res.push_back(myAddress);
  res.push_back(pKey);
  res.push_back(testKey);
  return res;
}

Value sendSymmetric(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 2)
        throw runtime_error(
                "sendSymmetric <sender> <recipient>"
                + HelpRequiringPassphrase());

    EnsureWalletIsUnlocked();

    string myAddress = params[0].get_str();
    string strRecipientAddress = params[1].get_str();

    CBitcoinAddress addr;
    int r = checkAddress(myAddress, addr);
    if(r<0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if(!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CBitcoinAddress aRecipient;
    r = checkAddress(strRecipientAddress, aRecipient);
    if(r < 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid recipient address");

    strRecipientAddress = aRecipient.ToString();

    string rsaPubKeyStr = "";
    if(!pwalletMain->envCP1(key.GetPubKey(), rsaPubKeyStr))
        throw JSONRPCError(RPC_WALLET_ERROR, "no rsa key available for address");

    CDataStream ss(SER_GETHASH, 0);
    ss << rsaPubKeyStr;

    vector<unsigned char> vchSig;
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    const vchType vchSender = vchFromValue(myAddress);
    const vchType vchRecipient = vchFromValue(strRecipientAddress);

    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    CScript scriptPubKeyOrig;
    CScript scriptPubKey;


        uint160 hash160;
        bool isValid = AddressToHash160(strRecipientAddress, hash160);
        if(!isValid)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid dions address");
        scriptPubKeyOrig.SetBitcoinAddress(strRecipientAddress);

    const vchType vchKey = vchFromValue(rsaPubKeyStr);
    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

    scriptPubKey << OP_PUBLIC_KEY << vchSender << vchRecipient << vchKey << vchFromString(sigBase64) << OP_2DROP << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

       string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);

        if(strError != "")
        {
    LEAVE_CRITICAL_SECTION(cs_main)
          throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }

    }
    LEAVE_CRITICAL_SECTION(cs_main)



    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    res.push_back(sigBase64);
    return res;

}
bool getImportedPubKey(string foreignAddr, vchType& recipientPubKeyVch)
{
  ENTER_CRITICAL_SECTION(cs_main)
  {
    ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
    {
      BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
      {
        const CWalletTx& tx = item.second;

        vchType vchSender, vchRecipient, vchKey, vchAes, vchSig;
        int nOut;
        if(!tx.GetPublicKeyUpdate(nOut, vchSender, vchRecipient, vchKey, vchAes, vchSig))
          continue;

        string senderAddr = stringFromVch(vchSender);
        if(senderAddr == foreignAddr)
        {
          recipientPubKeyVch = vchKey;
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
  LEAVE_CRITICAL_SECTION(cs_main)
          return true;
        }
      }
    }
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
  }
  LEAVE_CRITICAL_SECTION(cs_main)

  return false;
}

bool getImportedPubKey(string myAddress, string foreignAddr, vchType& recipientPubKeyVch, vchType& aesKeyBase64EncryptedVch)
{
  ENTER_CRITICAL_SECTION(cs_main)
  {
    ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
    {
      BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
      {
        const CWalletTx& tx = item.second;

        vchType vchSender, vchRecipient, vchKey, vchAes, vchSig;
        int nOut;
        if(!tx.GetPublicKeyUpdate(nOut, vchSender, vchRecipient, vchKey, vchAes, vchSig))
          continue;

        string keyRecipientAddr = stringFromVch(vchRecipient);
        if(keyRecipientAddr == myAddress)
        {
          recipientPubKeyVch = vchKey;
          aesKeyBase64EncryptedVch = vchAes;
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
  LEAVE_CRITICAL_SECTION(cs_main)
          return true;
        }
      }
    }
    LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
  }
  LEAVE_CRITICAL_SECTION(cs_main)

  return false;
}

int checkAddress(string addr, CBitcoinAddress& a)
{
  CBitcoinAddress address(addr);
  if(!address.IsValid())
  {
    vector<AliasIndex> vtxPos;
    LocatorNodeDB ln1Db("r");
    std::transform(addr.begin(), addr.end(), addr.begin(), ::tolower);
    vchType vchAlias = vchFromString(addr);
    if(ln1Db.lKey(vchAlias))
    {
      if(!ln1Db.lGet(vchAlias, vtxPos))
        return -2;
      if(vtxPos.empty())
        return -3;

      AliasIndex& txPos = vtxPos.back();
      address.SetString(txPos.vAddress); 
      if(!address.IsValid())
        return -4;
    }
    else
    {
      return -1;
    }
  }
  
  a = address;
  return 0;
}

Value sendPublicKey(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 2)
        throw runtime_error(
                "sendPublicKey <sender> <recipient>"
                + HelpRequiringPassphrase());

    EnsureWalletIsUnlocked();

    string myAddress = params[0].get_str();
    string strRecipientAddress = params[1].get_str();

    CBitcoinAddress addr;
    int r = checkAddress(myAddress, addr);
    if(r < 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CBitcoinAddress aRecipient;
    r = checkAddress(strRecipientAddress, aRecipient);
    if(r < 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid recipient address");

    strRecipientAddress = aRecipient.ToString();

    CKeyID keyID;
    if(!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CPubKey vchPubKey;
    pwalletMain->GetPubKey(keyID, vchPubKey);

    string rsaPubKeyStr = "";
    if(!pwalletMain->envCP1(key.GetPubKey(), rsaPubKeyStr))
        throw JSONRPCError(RPC_WALLET_ERROR, "no rsa key available for address");

    const vchType vchKey = vchFromValue(rsaPubKeyStr);
    const vchType vchSender = vchFromValue(myAddress);
    const vchType vchRecipient = vchFromValue(strRecipientAddress);
    vector<unsigned char> vchSig;
    CDataStream ss(SER_GETHASH, 0);
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");
    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    CScript scriptPubKeyOrig;
    CScript scriptPubKey;

    uint160 hash160;
    bool isValid = AddressToHash160(strRecipientAddress, hash160);
    if(!isValid)
      throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid dions address");

    vchType recipientPubKeyVch;
    vchType recipientAESKeyVch;
    vchType aes256Key;

    string encrypted;
    if(getImportedPubKey(myAddress, strRecipientAddress, recipientPubKeyVch, recipientAESKeyVch))
    {
      GenerateAESKey(aes256Key);


      string aesKeyStr = EncodeBase64(&aes256Key[0], aes256Key.size());

      const string publicKeyStr = stringFromVch(recipientPubKeyVch);
      EncryptMessage(publicKeyStr, aesKeyStr, encrypted);

      CWalletDB walletdb(pwalletMain->strWalletFile, "r+");
      if(!pwalletMain->SetAESMetadata(vchPubKey, aesKeyStr))
        throw JSONRPCError(RPC_TYPE_ERROR, "Failed to set meta data for key");

      if(!walletdb.UpdateKey(vchPubKey, pwalletMain->mapKeyMetadata[vchPubKey.GetID()]))
        throw JSONRPCError(RPC_TYPE_ERROR, "Failed to write meta data for key");

      ss <<(rsaPubKeyStr + encrypted);
      scriptPubKey << OP_PUBLIC_KEY << vchSender << vchRecipient << vchKey
                   << vchFromString(encrypted)
                   << vchFromString(sigBase64)
                   << OP_2DROP << OP_2DROP << OP_2DROP;
    }
    else
    {
      ss << rsaPubKeyStr + "I";
      scriptPubKey << OP_PUBLIC_KEY << vchSender << vchRecipient << vchKey
                   << vchFromString("I")
                   << vchFromString(sigBase64)
                   << OP_2DROP << OP_2DROP << OP_2DROP;
    } 


    scriptPubKeyOrig.SetBitcoinAddress(strRecipientAddress);


    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

       string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);

        if(strError != "")
        {
          LEAVE_CRITICAL_SECTION(cs_main)
          throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }

    }
    LEAVE_CRITICAL_SECTION(cs_main)



    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    res.push_back(sigBase64);
    return res;

}
Value sendPlainMessage(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 3)
        throw runtime_error(
                "sendPlainMessage <sender_address> <message> <recipient_address>"
                + HelpRequiringPassphrase());

    const string myAddress = params[0].get_str();
    const string strMessage = params[1].get_str();
    const string strRecipientAddress = params[2].get_str();

    CBitcoinAddress senderAddr(myAddress);
    if(!senderAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid sender address");

    CKeyID keyID;
    if(!senderAddr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "senderAddr does not refer to key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CBitcoinAddress recipientAddr(strRecipientAddress);
    if(!recipientAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid recipient address");

    CKeyID rkeyID;
    if(!recipientAddr.GetKeyID(rkeyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "recipientAddr does not refer to key");

    CDataStream ss(SER_GETHASH, 0);
    ss << strMessage;

    vector<unsigned char> vchSig;
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    CScript scriptPubKeyOrig;
    CScript scriptPubKey;


        uint160 hash160;
        bool isValid = AddressToHash160(strRecipientAddress, hash160);
        if(!isValid)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid dions address");
        scriptPubKeyOrig.SetBitcoinAddress(strRecipientAddress);

    vchType vchMessage = vchFromString(strMessage);
    scriptPubKey << OP_MESSAGE << vchFromString(myAddress) << vchFromString(strRecipientAddress) << vchMessage << vchFromString(sigBase64) << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

       string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);

        if(strError != "")
          throw JSONRPCError(RPC_WALLET_ERROR, strError);
        mapMyMessages[vchMessage] = wtx.GetHash();
    }
    LEAVE_CRITICAL_SECTION(cs_main)



    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    return res;

}
Value sendMessage(const Array& params, bool fHelp)
{
    if(fHelp || params.size() > 3)
        throw runtime_error(
                "sendMessage <sender_address> <message> <recipient_address>"
                + HelpRequiringPassphrase());

    string myAddress = params[0].get_str();
    string strMessage = params[1].get_str();
    string strRecipientAddress = params[2].get_str();

    CBitcoinAddress recipientAddr(strRecipientAddress);
    if(!recipientAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid recipient address");

    CKeyID rkeyID;
    if(!recipientAddr.GetKeyID(rkeyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "recipientAddr does not refer to key");

    vchType recipientAddressVch = vchFromString(strRecipientAddress);
    vchType recipientPubKeyVch;
    vector<unsigned char> aesRawVector;

    CKey key;
    if(params.size() == 3)
    {

      CBitcoinAddress senderAddr(myAddress);
      if(!senderAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid sender address");

      CKeyID keyID;
      if(!senderAddr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "senderAddr does not refer to key");

      if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

      CPubKey vchPubKey;
      pwalletMain->GetPubKey(keyID, vchPubKey);

      string aesBase64Plain;
      if(pwalletMain->GetAESMetadata(vchPubKey, aesBase64Plain))
      {
        bool fInvalid = false;
        aesRawVector = DecodeBase64(aesBase64Plain.c_str(), &fInvalid);

      }
      else
      {
        vchType aesKeyBase64EncryptedVch;
        if(getImportedPubKey(myAddress, strRecipientAddress, recipientPubKeyVch, aesKeyBase64EncryptedVch))
        {
          string aesKeyBase64Encrypted = stringFromVch(aesKeyBase64EncryptedVch);

          string privRSAKey;
          if(!pwalletMain->envCP0(vchPubKey, privRSAKey))
            throw JSONRPCError(RPC_TYPE_ERROR, "Failed to retrieve private RSA key");

          string decryptedAESKeyBase64;
          DecryptMessage(privRSAKey, aesKeyBase64Encrypted, decryptedAESKeyBase64);
          bool fInvalid = false;
          aesRawVector = DecodeBase64(decryptedAESKeyBase64.c_str(), &fInvalid);
        }
        else
        {
          throw JSONRPCError(RPC_WALLET_ERROR, "No local symmetric key and no imported symmetric key found for recipient");
        }
      }
    }

    string encrypted;
    string iv128Base64;
    EncryptMessageAES(strMessage, encrypted, aesRawVector, iv128Base64);

    CDataStream ss(SER_GETHASH, 0);
    ss << encrypted + iv128Base64;

    vector<unsigned char> vchSig;
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    CScript scriptPubKeyOrig;
    CScript scriptPubKey;

    uint160 hash160;
    bool isValid = AddressToHash160(strRecipientAddress, hash160);
    if(!isValid)
      throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid dions address");
    scriptPubKeyOrig.SetBitcoinAddress(strRecipientAddress);

    vchType vchEncryptedMessage = vchFromString(encrypted);
    vchType iv128Base64Vch = vchFromString(iv128Base64);
    scriptPubKey << OP_ENCRYPTED_MESSAGE << vchFromString(myAddress) << vchFromString(strRecipientAddress) << vchEncryptedMessage << iv128Base64Vch << vchFromString(sigBase64) << OP_2DROP << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

       string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);

        if(strError != "")
        {
    LEAVE_CRITICAL_SECTION(cs_main)
          throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
        mapMyMessages[vchEncryptedMessage] = wtx.GetHash();
    }
    LEAVE_CRITICAL_SECTION(cs_main)



    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    return res;

}
bool sign_verifymessage(string address)
{
  string message = "test";
    CBitcoinAddress ownerAddr(address);
    if(!ownerAddr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

    CKeyID keyID;
    if(!ownerAddr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CDataStream ss(SER_GETHASH, 0);
    ss << message;

    vector<unsigned char> vchSig;
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

    CBitcoinAddress addr(address);
    if(!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID__;
    if(!addr.GetKeyID(keyID__))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    bool fInvalid = false;
    vector<unsigned char> vchSig__ = DecodeBase64(sigBase64.c_str(), &fInvalid);

    if(fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CDataStream ss__(SER_GETHASH, 0);
    ss__ << message;

    CKey key__;
    if(!key__.SetCompactSignature(Hash(ss__.begin(), ss__.end()), vchSig__))
        return false;

    return(key__.GetPubKey().GetID() == keyID__);
}
Value registerAliasGenerate(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 1)
        throw runtime_error(
                "registerAliasGenerate <alias>"
                + HelpRequiringPassphrase());

    string locatorStr = params[0].get_str();

    locatorStr = stripSpacesAndQuotes(locatorStr);

    if(isOnlyWhiteSpace(locatorStr))
    {
      string err = "Attempt to register alias consisting only of white space";

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }
    else if(locatorStr.size() > 255)
    {
      string err = "Attempt to register alias more than 255 chars";

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }

    std::transform(locatorStr.begin(), locatorStr.end(), locatorStr.begin(), ::tolower);
    uint256 wtxInHash__;
    if(searchAliasEncrypted2(locatorStr, wtxInHash__) == true)
    {
      string err = "Attempt to register alias : " + locatorStr + ", this alias is already registered as encrypted with tx " + wtxInHash__.GetHex();

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }

    LocatorNodeDB aliasCacheDB("r");
    CTransaction tx;
    if(aliasTx(aliasCacheDB, vchFromString(locatorStr), tx))
    {
      string err = "Attempt to register alias : " + locatorStr + ", this alias is already active with tx " + tx.GetHash().GetHex();

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }

    const uint64_t rand = GetRand((uint64_t)-1);
    const vchType vchRand = CBigNum(rand).getvch();
    vchType vchToHash(vchRand);

    const vchType vchAlias = vchFromValue(locatorStr);
    vchToHash.insert(vchToHash.end(), vchAlias.begin(), vchAlias.end());
    const uint160 hash = Hash160(vchToHash);

    CPubKey vchPubKey;
    CReserveKey reservekey(pwalletMain);
    if(!reservekey.GetReservedKey(vchPubKey))
    {
      return false;
    }

    reservekey.KeepKey();

    CBitcoinAddress keyAddress(vchPubKey.GetID());
    CKeyID keyID;
    keyAddress.GetKeyID(keyID);
    pwalletMain->SetAddressBookName(keyID, "");
  string testKey;

  CWalletDB walletdb(pwalletMain->strWalletFile, "r+");
  if(!pwalletMain->SetRSAMetadata(vchPubKey))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to load meta data for key");

  if(!walletdb.UpdateKey(vchPubKey, pwalletMain->mapKeyMetadata[vchPubKey.GetID()]))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to write meta data for key");

  string pKey;
  if(!pwalletMain->envCP0(vchPubKey, pKey))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to load meta data for key");

  string pub_k;
  if(!pwalletMain->envCP1(vchPubKey, pub_k))
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "address has no associated RSA keys");

  if(!pwalletMain->SetRandomKeyMetadata(vchPubKey, vchRand))
    throw JSONRPCError(RPC_WALLET_ERROR, "Failed to set meta data for key");

  if(!walletdb.UpdateKey(vchPubKey, pwalletMain->mapKeyMetadata[vchPubKey.GetID()]))
    throw JSONRPCError(RPC_TYPE_ERROR, "Failed to write meta data for key");

  CKey key;
  if(!pwalletMain->GetKey(keyID, key))
    throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

  string encrypted;
  EncryptMessage(pub_k, locatorStr, encrypted);

  CWalletTx wtx;
  wtx.nVersion = CTransaction::DION_TX_VERSION;

  CScript scriptPubKeyOrig;
  scriptPubKeyOrig.SetBitcoinAddress(vchPubKey.Raw());
  CScript scriptPubKey;
  vchType vchEncryptedAlias = vchFromString(encrypted);
  string tmp = stringFromVch(vchEncryptedAlias);
  vchType vchValue = vchFromString("Q1JFQVRFRA==");

  CDataStream ss(SER_GETHASH, 0);
  ss << encrypted;
  ss << hash.ToString();
  ss << stringFromVch(vchValue);
  ss << string("0");

    vector<unsigned char> vchSig;
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());

    scriptPubKey << OP_ALIAS_ENCRYPTED << vchEncryptedAlias << vchFromString(sigBase64) << vchFromString(keyAddress.ToString()) << hash << vchValue << vchFromString("0") << OP_2DROP << OP_2DROP << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

       string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);

        if(strError != "")
        {
          LEAVE_CRITICAL_SECTION(cs_main)
          throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
        mapLocator[vchAlias] = wtx.GetHash();
    }
    LEAVE_CRITICAL_SECTION(cs_main)

    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    return res;
}
Value registerAlias(const Array& params, bool fHelp)
{
    if(fHelp || params.size() != 2)
        throw runtime_error(
                "registerAlias <alias> <address>"
                + HelpRequiringPassphrase());

    string locatorStr = params[0].get_str();

    locatorStr = stripSpacesAndQuotes(locatorStr);
    if(isOnlyWhiteSpace(locatorStr))
    {
      string err = "Attempt to register alias consisting only of white space";

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }
    else if(locatorStr.size() > 255)
    {
      string err = "Attempt to register alias more than 255 chars";

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }

    std::transform(locatorStr.begin(), locatorStr.end(), locatorStr.begin(), ::tolower);
    uint256 wtxInHash__;
    if(searchAliasEncrypted2(locatorStr, wtxInHash__) == true)
    {
      string err = "Attempt to register alias : " + locatorStr + ", this alias is already registered as encrypted with tx " + wtxInHash__.GetHex();

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }

    LocatorNodeDB aliasCacheDB("r");
    CTransaction tx;
    if(aliasTx(aliasCacheDB, vchFromString(locatorStr), tx))
    {
      string err = "Attempt to register alias : " + locatorStr + ", this alias is already active with tx " + tx.GetHash().GetHex();

      throw JSONRPCError(RPC_WALLET_ERROR, err);
    }

    const std::string strAddress = params[1].get_str();

    const uint64_t rand = GetRand((uint64_t)-1);
    const vchType vchRand = CBigNum(rand).getvch();
    vchType vchToHash(vchRand);

    const vchType vchAlias = vchFromValue(locatorStr);
    vchToHash.insert(vchToHash.end(), vchAlias.begin(), vchAlias.end());
    const uint160 hash = Hash160(vchToHash);

    uint160 hash160;
    bool isValid = AddressToHash160(strAddress, hash160);
    if(!isValid)
      throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
        "Invalid dions address");

    CBitcoinAddress keyAddress(strAddress);
    CKeyID keyID;
    keyAddress.GetKeyID(keyID);
    CPubKey vchPubKey;
    pwalletMain->GetPubKey(keyID, vchPubKey);
    string pub_k;
    if(!pwalletMain->envCP1(vchPubKey, pub_k))
      throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "address has no associated RSA keys");

    if(!pwalletMain->SetRandomKeyMetadata(vchPubKey, vchRand))
      throw JSONRPCError(RPC_WALLET_ERROR, "Failed to set meta data for key");

    CWalletDB walletdb(pwalletMain->strWalletFile, "r+");

    if(!walletdb.UpdateKey(vchPubKey, pwalletMain->mapKeyMetadata[vchPubKey.GetID()]))
      throw JSONRPCError(RPC_TYPE_ERROR, "Failed to write meta data for key");

    CKey key;
    if(!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");


    string encrypted;
    EncryptMessage(pub_k, locatorStr, encrypted);


    CWalletTx wtx;
    wtx.nVersion = CTransaction::DION_TX_VERSION;

    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetBitcoinAddress(vchPubKey.Raw());
    CScript scriptPubKey;
    vchType vchEncryptedAlias = vchFromString(encrypted);
    string tmp = stringFromVch(vchEncryptedAlias);

    vchType vchValue = vchFromString("Q1JFQVRFRA==");

    CDataStream ss(SER_GETHASH, 0);
    ss << encrypted;
    ss << hash.ToString();
    ss << stringFromVch(vchValue);
    ss << string("0");

    vector<unsigned char> vchSig;
    if(!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    string sigBase64 = EncodeBase64(&vchSig[0], vchSig.size());
    scriptPubKey << OP_ALIAS_ENCRYPTED << vchEncryptedAlias << vchFromString(sigBase64) << vchFromString(strAddress) << hash << vchValue << vchFromString("0") << OP_2DROP << OP_2DROP << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    ENTER_CRITICAL_SECTION(cs_main)
    {
        EnsureWalletIsUnlocked();

       string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);

        if(strError != "")
        {
    LEAVE_CRITICAL_SECTION(cs_main)
          throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
        mapLocator[vchAlias] = wtx.GetHash();
    }
    LEAVE_CRITICAL_SECTION(cs_main)


    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    return res;
}
bool aliasTxPos(const vector<AliasIndex> &vtxPos, const CDiskTxPos& txPos)
{
    if(vtxPos.empty())
        return false;

    return vtxPos.back().txPos == txPos;
}
bool aliasScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch)
{

  CScript::const_iterator pc = script.begin();
  bool r = aliasScript(script, op, vvch, pc);

  return r;
}
bool aliasScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc)
{

    opcodetype opcode;
    if(!script.GetOp(pc, opcode))
        return false;
    if(opcode < OP_1 || opcode > OP_16)
        return false;

    op = opcode - OP_1 + 1;

    for(;;) {
        vector<unsigned char> vch;
        if(!script.GetOp(pc, opcode, vch))
            return false;
        if(opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
            break;
        if(!(opcode >= 0 && opcode <= OP_PUSHDATA4))
            return false;
        vvch.push_back(vch);
    }

    while(opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
    {
        if(!script.GetOp(pc, opcode))
            break;
    }

    pc--;

    if((op == OP_ALIAS_ENCRYPTED && vvch.size() == 6) ||
       (op == OP_ALIAS_SET && vvch.size() == 5) ||
           (op == OP_MESSAGE) ||
           (op == OP_ENCRYPTED_MESSAGE) ||
           (op == OP_PUBLIC_KEY) ||
           (op == OP_ALIAS_RELAY && vvch.size() == 2))
        return true;
    return error("invalid number of arguments for alias op");
}
bool DecodeMessageTx(const CTransaction& tx, int& op, int& nOut, vector<vector<unsigned char> >& vvch )
{
    bool found = false;

        for(unsigned int i = 0; i < tx.vout.size(); i++)
        {
            const CTxOut& out = tx.vout[i];

            vector<vector<unsigned char> > vvchRead;

            if(aliasScript(out.scriptPubKey, op, vvchRead))
            {
                if(found)
                {
                    vvch.clear();
                    return false;
                }
                nOut = i;
                found = true;
                vvch = vvchRead;
            }
        }

        if(!found)
            vvch.clear();

    return found;
}
bool aliasTx(const CTransaction& tx, int& op, int& nOut, vector<vector<unsigned char> >& vvch )
{
    bool found = false;

        for(unsigned int i = 0; i < tx.vout.size(); i++)
        {
            const CTxOut& out = tx.vout[i];

            vector<vector<unsigned char> > vvchRead;

            if(aliasScript(out.scriptPubKey, op, vvchRead))
            {
                if(found)
                {
                    vvch.clear();
                    return false;
                }
                nOut = i;
                found = true;
                vvch = vvchRead;
            }
        }

        if(!found)
            vvch.clear();

    return found;
}
bool aliasTxValue(const CTransaction& tx, vector<unsigned char>& value)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    if(!aliasTx(tx, op, nOut, vvch))
        return false;

    switch(op)
    {
        case OP_ALIAS_ENCRYPTED:
            return false;
        case OP_ALIAS_SET:
            value = vvch[2];
            return true;
        case OP_ALIAS_RELAY:
            value = vvch[1];
            return true;
        default:
            return false;
    }
}
int aliasOutIndex(const CTransaction& tx)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = aliasTx(tx, op, nOut, vvch);

    if(!good)
        throw runtime_error("aliasOutIndex() : alias output not found");
    return nOut;
}
bool IsMinePost(const CTransaction& tx)
{
    if(tx.nVersion != CTransaction::DION_TX_VERSION)
        return false;

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = aliasTx(tx, op, nOut, vvch);

    if(!good)
    {
        error("IsMinePost() : no output out script in alias tx %s\n", tx.ToString().c_str());
        return false;
    }

    const CTxOut& txout = tx.vout[nOut];
    if(IsLocator(tx, txout))
    {
        return true;
    }
    return false;
}

bool verifymessage(const string& strAddress, const string& strSig, const string& m1, const string& m2, const string& m3, const string& m4)
{
    CBitcoinAddress addr(strAddress);
    if(!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if(!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    bool fInvalid = false;
    vector<unsigned char> vchSig = DecodeBase64(strSig.c_str(), &fInvalid);

    if(fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CDataStream ss(SER_GETHASH, 0);
    ss << m1;
    ss << m2;
    ss << m3;
    ss << m4;

    CKey key;
    if(!key.SetCompactSignature(Hash(ss.begin(), ss.end()), vchSig))
        return false;

    return(key.GetPubKey().GetID() == keyID);
}
bool verifymessage(const string& strAddress, const string& strSig, const string& strMessage)
{
    CBitcoinAddress addr(strAddress);
    if(!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if(!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    bool fInvalid = false;
    vector<unsigned char> vchSig = DecodeBase64(strSig.c_str(), &fInvalid);

    if(fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CDataStream ss(SER_GETHASH, 0);
    ss << strMessage;

    CKey key;
    if(!key.SetCompactSignature(Hash(ss.begin(), ss.end()), vchSig))
        return false;

    return(key.GetPubKey().GetID() == keyID);
}
bool IsMinePost(const CTransaction& tx, const CTxOut& txout, bool ignore_registerAlias )
{
    if(tx.nVersion != CTransaction::DION_TX_VERSION)
        return false;

    vector<vector<unsigned char> > vvch;

    int op;

    if(!aliasScript(txout.scriptPubKey, op, vvch))
        return false;

    if(ignore_registerAlias && op == OP_ALIAS_ENCRYPTED)
        return false;

    if(IsLocator(tx, txout))
    {

        return true;
    }
    return false;
}

bool
AcceptToMemoryPoolPost(const CTransaction& tx)
{
    if(tx.nVersion != CTransaction::DION_TX_VERSION)
        return true;

    if(tx.vout.size() < 1)
      return error("AcceptToMemoryPoolPost: no output in alias tx %s\n",
                    tx.GetHash().ToString().c_str());

    std::vector<vchType> vvch;

    int op;
    int nOut;

    bool good = aliasTx(tx, op, nOut, vvch);

    if(!good)
      return error("AcceptToMemoryPoolPost: no output out script in alias tx %s",
                    tx.GetHash().ToString().c_str());



    if(op == OP_ALIAS_ENCRYPTED)
      {
        const vchType& hash = vvch[0];
# 2591 "dions.cpp"
        if(setNewHashes.count(hash) > 0)
          return error("AcceptToMemoryPoolPost: duplicate registerAlias hash in tx %s",
                        tx.GetHash().ToString().c_str());
        setNewHashes.insert(hash);
      }
    else
    {
      ENTER_CRITICAL_SECTION(cs_main)
      {
        mapState[vvch[0]].insert(tx.GetHash());
      }
      LEAVE_CRITICAL_SECTION(cs_main)
    }

    return true;
}
void RemoveFromMemoryPoolPost(const CTransaction& tx)
{
    if(tx.nVersion != CTransaction::DION_TX_VERSION)
        return;

    if(tx.vout.size() < 1)
        return;

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    if(!aliasTx(tx, op, nOut, vvch))
        return;

    if(op != OP_ALIAS_ENCRYPTED)
    {
        ENTER_CRITICAL_SECTION(cs_main)
        {
            std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi = mapState.find(vvch[0]);
            if(mi != mapState.end())
                mi->second.erase(tx.GetHash());
        }
        LEAVE_CRITICAL_SECTION(cs_main)
    }
}

int CheckTransactionAtRelativeDepth(CBlockIndex* pindexBlock, CTxIndex& txindex, int maxDepth)
{
    for(CBlockIndex* pindex = pindexBlock; pindex && pindexBlock->nHeight - pindex->nHeight < maxDepth; pindex = pindex->pprev)
        if(pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
            return pindexBlock->nHeight - pindex->nHeight;
    return -1;
}
bool
ConnectInputsPost(map<uint256, CTxIndex>& mapTestPool,
                               const CTransaction& tx,
                               vector<CTransaction>& vTxPrev,
                               vector<CTxIndex>& vTxindex,
                               CBlockIndex* pindexBlock, CDiskTxPos& txPos,
                               bool fBlock, bool fMiner)
{
    LocatorNodeDB ln1Db("r+");
    int nInput;
    bool found = false;

    int prevOp;
    std::vector<vchType> vvchPrevArgs;

    for(int i = 0; i < tx.vin.size(); i++)
    {
      const CTxOut& out = vTxPrev[i].vout[tx.vin[i].prevout.n];
      std::vector<vchType> vvchPrevArgsRead;

      if(aliasScript(out.scriptPubKey, prevOp, vvchPrevArgsRead))
      {
        if(found)
          return error("ConnectInputsPost() : multiple previous alias transactions");
        found = true;
        nInput = i;

        vvchPrevArgs = vvchPrevArgsRead;
      }
    }
    if(tx.nVersion != CTransaction::DION_TX_VERSION)
    {

        bool found= false;
        for(int i = 0; i < tx.vout.size(); i++)
        {
            const CTxOut& out = tx.vout[i];

            std::vector<vchType> vvchRead;
            int opRead;

            if(aliasScript(out.scriptPubKey, opRead, vvchRead))
                found=true;
        }

        if(found)
            return error("ConnectInputsPost() : a non-dions transaction with a dions input");
        return true;
    }

    std::vector<vchType> vvchArgs;
    int op;
    int nOut;

    bool good = aliasTx(tx, op, nOut, vvchArgs);
    if(!good)
        return error("ConnectInputsPost() : could not decode a dions tx");

    CScript s1 = tx.vout[nOut].scriptPubKey;
    const CScript& s1_ = aliasStrip(s1);
    string a1 = s1_.GetBitcoinAddress();
    

    int nPrevHeight;
    int nDepth;

    if(tx.vout[nOut].nValue < MIN_AMOUNT)
      {
        if(!fBlock )
          return error("ConnectInputsPost: not enough locked amount");
      }

    if(vvchArgs[0].size() > MAX_LOCATOR_LENGTH)
        return error("alias transaction with alias too long");

    switch(op)
    {
        case OP_PUBLIC_KEY:
        {
           const std::string sender(vvchArgs[0].begin(), vvchArgs[0].end());
           const std::string recipient(vvchArgs[1].begin(), vvchArgs[1].end());
           const std::string pkey(vvchArgs[2].begin(), vvchArgs[2].end());
           const std::string aesEncrypted(stringFromVch(vvchArgs[3]));
           const std::string sig(stringFromVch(vvchArgs[4]));

           CBitcoinAddress addr(sender);

           if(!addr.IsValid())
             throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

           CKeyID keyID;
           if(!addr.GetKeyID(keyID))
               throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

           bool fInvalid = false;
          vector<unsigned char> vchSig = DecodeBase64(sig.c_str(), &fInvalid);

           if(fInvalid)
               throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

           CDataStream ss(SER_GETHASH, 0);
           ss << pkey + aesEncrypted;

           CKey key;
           if(!key.SetCompactSignature(Hash(ss.begin(), ss.end()), vchSig))
               return false;

           if(key.GetPubKey().GetID() != keyID)
           {
                return error("public key plus aes key tx verification failed");
           }
        }
            break;
        case OP_ENCRYPTED_MESSAGE:
        {
           const std::string sender(vvchArgs[0].begin(), vvchArgs[0].end());
           const std::string recipient(vvchArgs[1].begin(), vvchArgs[1].end());
           const std::string encrypted(vvchArgs[2].begin(), vvchArgs[2].end());
           const std::string iv128Base64(stringFromVch(vvchArgs[3]));
           const std::string sig(stringFromVch(vvchArgs[4]));

           CBitcoinAddress addr(sender);

           if(!addr.IsValid())
             throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

           CKeyID keyID;
           if(!addr.GetKeyID(keyID))
               throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

           bool fInvalid = false;
          vector<unsigned char> vchSig = DecodeBase64(sig.c_str(), &fInvalid);

           if(fInvalid)
               throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

           CDataStream ss(SER_GETHASH, 0);
           ss << encrypted + iv128Base64;

           CKey key;
           if(!key.SetCompactSignature(Hash(ss.begin(), ss.end()), vchSig))
               return false;

           if(key.GetPubKey().GetID() != keyID)
           {
                return error("encrypted message tx verification failed");
           }
        }
            break;
        case OP_MESSAGE:
        {
           const std::string sender(vvchArgs[0].begin(), vvchArgs[0].end());
           const std::string recipient(vvchArgs[1].begin(), vvchArgs[1].end());
           const std::string message(vvchArgs[2].begin(), vvchArgs[2].end());
           const std::string sig(stringFromVch(vvchArgs[3]));

           CBitcoinAddress addr(sender);

           if(!addr.IsValid())
             throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

           CKeyID keyID;
           if(!addr.GetKeyID(keyID))
               throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

           bool fInvalid = false;
          vector<unsigned char> vchSig = DecodeBase64(sig.c_str(), &fInvalid);

           if(fInvalid)
               throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

           CDataStream ss(SER_GETHASH, 0);
           ss << message;

           CKey key;
           if(!key.SetCompactSignature(Hash(ss.begin(), ss.end()), vchSig))
               return false;

           if(key.GetPubKey().GetID() != keyID)
           {
                return error("encrypted message tx verification failed");
           }
        }
            break;
        case OP_ALIAS_ENCRYPTED:
        {

            if(vvchArgs[3].size() != 20)
              return error("registerAlias tx with incorrect hash length");
            CScript script;
            if(vvchPrevArgs.size() != 0)
              script.SetBitcoinAddress(stringFromVch(vvchPrevArgs[2]));
            else
              script.SetBitcoinAddress(stringFromVch(vvchArgs[2]));

              string encrypted = stringFromVch(vvchArgs[0]);
              uint160 hash = uint160(vvchArgs[3]);
              string value = stringFromVch(vvchArgs[4]);
              string r = stringFromVch(vvchArgs[5]);
            if(!verifymessage(script.GetBitcoinAddress(), stringFromVch(vvchArgs[1]), encrypted, hash.ToString(), value, r))
            {
              return error("Dions::ConnectInputsPost: failed to verify signature for registerAlias tx %s",
                      tx.GetHash().ToString().c_str());
            }

           
           if(r != "0" && IsMinePost(tx))
           {
             CScript script;
             script.SetBitcoinAddress(stringFromVch(vvchArgs[2]));

             CBitcoinAddress ownerAddr = script.GetBitcoinAddress();
             if(!ownerAddr.IsValid())
               throw JSONRPCError(RPC_TYPE_ERROR, "Invalid owner address");

             CKeyID keyID;
             if(!ownerAddr.GetKeyID(keyID))
               throw JSONRPCError(RPC_TYPE_ERROR, "ownerAddr does not refer to key");


             CKey key;
             if(!pwalletMain->GetKey(keyID, key))
               throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

             CPubKey vchPubKey;
             pwalletMain->GetPubKey(keyID, vchPubKey);

             bool fInvalid;
             string decryptedRand;

             string privRSAKey;
             ENTER_CRITICAL_SECTION(cs_main)
             {
               ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet)
               {
                 if(!pwalletMain->envCP0(vchPubKey, privRSAKey))
                 {
                   LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
                   LEAVE_CRITICAL_SECTION(cs_main)
                   throw JSONRPCError(RPC_TYPE_ERROR, "Failed to retrieve private RSA key");
                 }
               }
               LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet)
             }
             LEAVE_CRITICAL_SECTION(cs_main)

             DecryptMessage(privRSAKey, r, decryptedRand);

             vchType vchRand = DecodeBase64(decryptedRand.c_str(), &fInvalid);
             if(!pwalletMain->SetRandomKeyMetadata(vchPubKey, vchRand))
               throw JSONRPCError(RPC_WALLET_ERROR, "Failed to set meta data for key");
            CWalletDB walletdb(pwalletMain->strWalletFile, "r+");
            if(!walletdb.UpdateKey(vchPubKey, pwalletMain->mapKeyMetadata[vchPubKey.GetID()]))
              throw JSONRPCError(RPC_TYPE_ERROR, "Failed to write meta data for key");
           }
           
        }
            break;
        case OP_ALIAS_SET:
        {
            if(!found || prevOp != OP_ALIAS_ENCRYPTED)
                return error("ConnectInputsPost() : decryptAlias tx without previous registerAlias tx");

            CScript script;
            if(vvchPrevArgs.size() != 0)
              script.SetBitcoinAddress(stringFromVch(vvchPrevArgs[2]));
            else
              script.SetBitcoinAddress(stringFromVch(vvchArgs[2]));

            if(!verifymessage(script.GetBitcoinAddress(), stringFromVch(vvchArgs[1]), stringFromVch(vvchArgs[0])))
              return error("Dions::ConnectInputsPost: failed to verify signature for decryptAlias tx %s",
                      tx.GetHash().ToString().c_str());



             if(vvchArgs[2] != vvchPrevArgs[2])
             {
               return error("Dions::ConnectInputsPost: OP_ALIAS_SET owner address does not match registerAlias address");
             }

            if(vvchArgs[3].size() > 20)
                return error("decryptAlias tx with rand too big");
            
            if(vvchArgs[2].size() > MAX_XUNIT_LENGTH)
                return error("decryptAlias tx with value too long");

            {
                const vchType& vchHash = vvchPrevArgs[3];
                const vchType& vchAlias = vvchArgs[0];
                const vchType& vchRand = vvchArgs[3];
                vchType vchToHash(vchRand);
                vchToHash.insert(vchToHash.end(), vchAlias.begin(), vchAlias.end());
                uint160 hash = Hash160(vchToHash);
                if(uint160(vchHash) != hash)
                {
                        return error("ConnectInputsPost() : decryptAlias hash mismatch");
                }
            }

            nPrevHeight = aliasHeight(vvchArgs[0]);
            if(nPrevHeight >= 0 && pindexBlock->nHeight - nPrevHeight < GetExpirationDepth(pindexBlock->nHeight))
                return error("ConnectInputsPost() : decryptAlias on an unexpired alias");
            nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], MIN_SET_DEPTH);

            if((fBlock || fMiner) && nDepth >= 0 && nDepth < MIN_SET_DEPTH)
                return false;



            if(fMiner)
            {

                nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], GetExpirationDepth(pindexBlock->nHeight));
                if(nDepth == -1)
                    return error("ConnectInputsPost() : decryptAlias cannot be mined if registerAlias is not already in chain and unexpired");

                set<uint256>& setPending = mapState[vvchArgs[0]];
                BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& s, mapTestPool)
                {
                    if(setPending.count(s.first))
                    {
                        return false;
                    }
                }
            }
        }
            break;
        case OP_ALIAS_RELAY:
            if(!found ||(prevOp != OP_ALIAS_SET && prevOp != OP_ALIAS_RELAY))
                return error("updateAlias tx without previous update tx");

            if(vvchArgs[1].size() > MAX_XUNIT_LENGTH)
                return error("updateAlias tx with value too long");

            if(vvchPrevArgs[0] != vvchArgs[0])
            {
                    return error("ConnectInputsPost() : updateAlias alias mismatch");
            }

            nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], GetExpirationDepth(pindexBlock->nHeight));
            if((fBlock || fMiner) && nDepth < 0)
                return error("ConnectInputsPost() : updateAlias on an expired alias, or there is a pending transaction on the alias");
            break;
        default:
            return error("ConnectInputsPost() : alias transaction has unknown op");
    }
    if(!fBlock && op == OP_ALIAS_RELAY)
    {
        vector<AliasIndex> vtxPos;
        if(ln1Db.lKey(vvchArgs[0])
            && !ln1Db.lGet(vvchArgs[0], vtxPos))
          return error("ConnectInputsPost() : failed to read from alias DB");
        if(!aliasTxPos(vtxPos, vTxindex[nInput].pos))
            return error("ConnectInputsPost() : tx %s rejected, since previous tx(%s) is not in the alias DB\n", tx.GetHash().ToString().c_str(), vTxPrev[nInput].GetHash().ToString().c_str());
    }
    if(fBlock)
    {
        if(op == OP_ALIAS_SET || op == OP_ALIAS_RELAY)
        {

            string locatorStr = stringFromVch(vvchArgs[0]);
            std::transform(locatorStr.begin(), locatorStr.end(), locatorStr.begin(), ::tolower);
            vector<AliasIndex> vtxPos;
            if(ln1Db.lKey(vchFromString(locatorStr))
                && !ln1Db.lGet(vchFromString(locatorStr), vtxPos))
            {
              return error("ConnectInputsPost() : failed to read from alias DB");
            }
            
            if(op == OP_ALIAS_SET)
            {
              CTransaction tx;
              if(aliasTx(ln1Db, vchFromString(locatorStr), tx))
              {
                return error("ConnectInputsPost() : this alias is already active with tx %s",
                tx.GetHash().GetHex().c_str());
              }
            }

            vector<unsigned char> vchValue;
            int nHeight;
            uint256 hash;
            GetValueOfTxPos(txPos, vchValue, hash, nHeight);
            AliasIndex txPos2;
            txPos2.nHeight = pindexBlock->nHeight;
            txPos2.vValue = vchValue;
            txPos2.vAddress = a1;
            txPos2.txPos = txPos;
            vtxPos.push_back(txPos2);
            if(!ln1Db.lPut(vvchArgs[0], vtxPos))
              return error("ConnectInputsPost() : failed to write to alias DB");
        }

        if(op != OP_ALIAS_ENCRYPTED)
        {
            ENTER_CRITICAL_SECTION(cs_main)
            {
                std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi = mapState.find(vvchArgs[0]);
                if(mi != mapState.end())
                    mi->second.erase(tx.GetHash());

            }
            LEAVE_CRITICAL_SECTION(cs_main)
         }
    }

    return true;
}
unsigned char GetAddressVersion() 
{ 
  return((unsigned char)(fTestNet ? 111 : 103)); 
}
