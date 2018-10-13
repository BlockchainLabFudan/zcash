// Copyright (c) 2018 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "transaction_builder.h"

#include "main.h"
#include "pubkey.h"
#include "script/sign.h"
#include "utilmoneystr.h"

#include <boost/variant.hpp>
#include <librustzcash.h>

SpendDescriptionInfo::SpendDescriptionInfo(
    libzcash::SaplingExpandedSpendingKey expsk,
    libzcash::SaplingNote note,
    uint256 anchor,
    SaplingWitness witness) : expsk(expsk), note(note), anchor(anchor), witness(witness)
{
    librustzcash_sapling_generate_r(alpha.begin());
}

TransactionBuilder::TransactionBuilder(
    const Consensus::Params& consensusParams,
    int nHeight,
    CKeyStore* keystore,
    ZCJoinSplit* sproutParams,
    CCoinsViewCache* coinsView,
    CCriticalSection* cs_coinsView) :
    consensusParams(consensusParams),
    nHeight(nHeight),
    keystore(keystore),
    sproutParams(sproutParams),
    coinsView(coinsView),
    cs_coinsView(cs_coinsView)
{
    mtx = CreateNewContextualCMutableTransaction(consensusParams, nHeight);
}

bool TransactionBuilder::AddSaplingSpend(
    libzcash::SaplingExpandedSpendingKey expsk,
    libzcash::SaplingNote note,
    uint256 anchor,
    SaplingWitness witness)
{
    // Consistency check: all anchors must equal the first one
    if (!spends.empty()) {
        if (spends[0].anchor != anchor) {
            return false;
        }
    }

    spends.emplace_back(expsk, note, anchor, witness);
    mtx.valueBalance += note.value();
    return true;
}

void TransactionBuilder::AddSaplingOutput(
    uint256 ovk,
    libzcash::SaplingPaymentAddress to,
    CAmount value,
    std::array<unsigned char, ZC_MEMO_SIZE> memo)
{
    auto note = libzcash::SaplingNote(to, value);
    outputs.emplace_back(ovk, note, memo);
    mtx.valueBalance -= value;
}

bool TransactionBuilder::AddSproutInput(
    libzcash::SproutSpendingKey sk,
    libzcash::SproutNote note,
    SproutWitness witness)
{
    if (sproutParams == nullptr) {
        throw std::runtime_error("Cannot add Sprout inputs to a TransactionBuilder without Sprout params");
    }

    // Consistency check: all anchors must equal the first one
    if (!jsInputs.empty()) {
        if (jsInputs[0].witness.root() != witness.root()) {
            return false;
        }
    }

    jsInputs.emplace_back(witness, note, sk);
    return true;
}

void TransactionBuilder::AddSproutOutput(
    libzcash::SproutPaymentAddress to,
    CAmount value,
    std::array<unsigned char, ZC_MEMO_SIZE> memo)
{
    if (sproutParams == nullptr) {
        throw std::runtime_error("Cannot add Sprout outputs to a TransactionBuilder without Sprout params");
    }

    libzcash::JSOutput jsOutput(to, value);
    jsOutput.memo = memo;
    jsOutputs.push_back(jsOutput);
}

void TransactionBuilder::AddTransparentInput(COutPoint utxo, CScript scriptPubKey, CAmount value)
{
    if (keystore == nullptr) {
        throw std::runtime_error("Cannot add transparent inputs to a TransactionBuilder without a keystore");
    }

    mtx.vin.emplace_back(utxo);
    tIns.emplace_back(scriptPubKey, value);
}

bool TransactionBuilder::AddTransparentOutput(CTxDestination& to, CAmount value)
{
    if (!IsValidDestination(to)) {
        return false;
    }

    CScript scriptPubKey = GetScriptForDestination(to);
    CTxOut out(value, scriptPubKey);
    mtx.vout.push_back(out);
    return true;
}

void TransactionBuilder::SetFee(CAmount fee)
{
    this->fee = fee;
}

void TransactionBuilder::SendChangeTo(libzcash::SaplingPaymentAddress changeAddr, uint256 ovk)
{
    saplingChangeAddr = std::make_pair(ovk, changeAddr);
    sproutChangeAddr = boost::none;
    tChangeAddr = boost::none;
}

void TransactionBuilder::SendChangeTo(libzcash::SproutPaymentAddress changeAddr)
{
    sproutChangeAddr = changeAddr;
    saplingChangeAddr = boost::none;
    tChangeAddr = boost::none;
}

bool TransactionBuilder::SendChangeTo(CTxDestination& changeAddr)
{
    if (!IsValidDestination(changeAddr)) {
        return false;
    }

    tChangeAddr = changeAddr;
    saplingChangeAddr = boost::none;
    sproutChangeAddr = boost::none;

    return true;
}

boost::optional<CTransaction> TransactionBuilder::Build()
{
    //
    // Consistency checks
    //

    // Valid change
    CAmount change = mtx.valueBalance - fee;
    for (auto jsInput : jsInputs) {
        change += jsInput.note.value();
    }
    for (auto jsOutput : jsOutputs) {
        change -= jsOutput.value;
    }
    for (auto tIn : tIns) {
        change += tIn.value;
    }
    for (auto tOut : mtx.vout) {
        change -= tOut.nValue;
    }
    if (change < 0) {
        return boost::none;
    }

    //
    // Change output
    //

    if (change > 0) {
        // Send change to the specified change address. If no change address
        // was set, send change to the first Sapling address given as input,
        // or the first Sprout address given as input.
        if (saplingChangeAddr) {
            AddSaplingOutput(saplingChangeAddr->first, saplingChangeAddr->second, change);
        } else if (sproutChangeAddr) {
            AddSproutOutput(sproutChangeAddr.get(), change);
        } else if (tChangeAddr) {
            // tChangeAddr has already been validated.
            assert(AddTransparentOutput(tChangeAddr.value(), change));
        } else if (!spends.empty()) {
            auto fvk = spends[0].expsk.full_viewing_key();
            auto note = spends[0].note;
            libzcash::SaplingPaymentAddress changeAddr(note.d, note.pk_d);
            AddSaplingOutput(fvk.ovk, changeAddr, change);
        } else if (!jsInputs.empty()) {
            auto changeAddr = jsInputs[0].key.address();
            AddSproutOutput(changeAddr, change);
        } else {
            return boost::none;
        }
    }

    //
    // Sapling spends and outputs
    //

    auto ctx = librustzcash_sapling_proving_ctx_init();

    // Create Sapling SpendDescriptions
    for (auto spend : spends) {
        auto cm = spend.note.cm();
        auto nf = spend.note.nullifier(
            spend.expsk.full_viewing_key(), spend.witness.position());
        if (!(cm && nf)) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return boost::none;
        }

        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << spend.witness.path();
        std::vector<unsigned char> witness(ss.begin(), ss.end());

        SpendDescription sdesc;
        if (!librustzcash_sapling_spend_proof(
                ctx,
                spend.expsk.full_viewing_key().ak.begin(),
                spend.expsk.nsk.begin(),
                spend.note.d.data(),
                spend.note.r.begin(),
                spend.alpha.begin(),
                spend.note.value(),
                spend.anchor.begin(),
                witness.data(),
                sdesc.cv.begin(),
                sdesc.rk.begin(),
                sdesc.zkproof.data())) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return boost::none;
        }

        sdesc.anchor = spend.anchor;
        sdesc.nullifier = *nf;
        mtx.vShieldedSpend.push_back(sdesc);
    }

    // Create Sapling OutputDescriptions
    for (auto output : outputs) {
        auto cm = output.note.cm();
        if (!cm) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return boost::none;
        }

        libzcash::SaplingNotePlaintext notePlaintext(output.note, output.memo);

        auto res = notePlaintext.encrypt(output.note.pk_d);
        if (!res) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return boost::none;
        }
        auto enc = res.get();
        auto encryptor = enc.second;

        OutputDescription odesc;
        if (!librustzcash_sapling_output_proof(
                ctx,
                encryptor.get_esk().begin(),
                output.note.d.data(),
                output.note.pk_d.begin(),
                output.note.r.begin(),
                output.note.value(),
                odesc.cv.begin(),
                odesc.zkproof.begin())) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return boost::none;
        }

        odesc.cm = *cm;
        odesc.ephemeralKey = encryptor.get_epk();
        odesc.encCiphertext = enc.first;

        libzcash::SaplingOutgoingPlaintext outPlaintext(output.note.pk_d, encryptor.get_esk());
        odesc.outCiphertext = outPlaintext.encrypt(
            output.ovk,
            odesc.cv,
            odesc.cm,
            encryptor);
        mtx.vShieldedOutput.push_back(odesc);
    }

    //
    // Sprout JoinSplits
    //

    unsigned char joinSplitPrivKey[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(mtx.joinSplitPubKey.begin(), joinSplitPrivKey);

    // Create Sprout JSDescriptions
    if (!jsInputs.empty() || !jsOutputs.empty()) {
        if (!CreateJSDescriptions()) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return boost::none;
        }
    }

    //
    // Signatures
    //

    auto consensusBranchId = CurrentEpochBranchId(nHeight, consensusParams);

    // Empty output script.
    uint256 dataToBeSigned;
    CScript scriptCode;
    try {
        dataToBeSigned = SignatureHash(scriptCode, mtx, NOT_AN_INPUT, SIGHASH_ALL, 0, consensusBranchId);
    } catch (std::logic_error ex) {
        librustzcash_sapling_proving_ctx_free(ctx);
        return boost::none;
    }

    // Create Sapling spendAuth and binding signatures
    for (size_t i = 0; i < spends.size(); i++) {
        librustzcash_sapling_spend_sig(
            spends[i].expsk.ask.begin(),
            spends[i].alpha.begin(),
            dataToBeSigned.begin(),
            mtx.vShieldedSpend[i].spendAuthSig.data());
    }
    librustzcash_sapling_binding_sig(
        ctx,
        mtx.valueBalance,
        dataToBeSigned.begin(),
        mtx.bindingSig.data());

    librustzcash_sapling_proving_ctx_free(ctx);

    // Create Sprout joinSplitSig
    if (crypto_sign_detached(
        mtx.joinSplitSig.data(), NULL,
        dataToBeSigned.begin(), 32,
        joinSplitPrivKey) != 0)
    {
        return boost::none;
    }

    // Sanity check Sprout joinSplitSig
    if (crypto_sign_verify_detached(
        mtx.joinSplitSig.data(),
        dataToBeSigned.begin(), 32,
        mtx.joinSplitPubKey.begin()) != 0)
    {
        return boost::none;
    }

    // Transparent signatures
    CTransaction txNewConst(mtx);
    for (int nIn = 0; nIn < mtx.vin.size(); nIn++) {
        auto tIn = tIns[nIn];
        SignatureData sigdata;
        bool signSuccess = ProduceSignature(
            TransactionSignatureCreator(
                keystore, &txNewConst, nIn, tIn.value, SIGHASH_ALL),
            tIn.scriptPubKey, sigdata, consensusBranchId);

        if (!signSuccess) {
            return boost::none;
        } else {
            UpdateTransaction(mtx, nIn, sigdata);
        }
    }

    return CTransaction(mtx);
}

bool TransactionBuilder::CreateJSDescriptions()
{
    // Copy jsInputs and jsOutputs to more flexible containers
    std::deque<libzcash::JSInput> jsInputsDeque;
    for (auto jsInput : jsInputs) {
        jsInputsDeque.push_back(jsInput);
    }
    std::deque<libzcash::JSOutput> jsOutputsDeque;
    for (auto jsOutput : jsOutputs) {
        jsOutputsDeque.push_back(jsOutput);
    }

    // If we have no Sprout shielded inputs, then we do the simpler more-leaky
    // process where we just create outputs directly. We save the chaining logic,
    // at the expense of leaking the sums of pairs of output values in vpub_old.
    if (jsInputs.empty()) {
        // Create joinsplits, where each output represents a zaddr recipient.
        while (jsOutputsDeque.size() > 0) {
            // Default array entries are dummy inputs and outputs
            std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin;
            std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout;
            uint64_t vpub_old = 0;

            for (int n = 0; n < ZC_NUM_JS_OUTPUTS && jsOutputsDeque.size() > 0; n++) {
                vjsout[n] = jsOutputsDeque.front();
                jsOutputsDeque.pop_front();

                // Funds are removed from the value pool and enter the private pool
                vpub_old += vjsout[n].value;
            }

            std::array<size_t, ZC_NUM_JS_INPUTS> inputMap;
            std::array<size_t, ZC_NUM_JS_OUTPUTS> outputMap;
            CreateJSDescription(vpub_old, 0, vjsin, vjsout, inputMap, outputMap);
        }
        return true;
    }

    // At this point, we are guaranteed to have at least one input note.
    // Use address of first input note as the temporary change address.
    auto changeKey = jsInputsDeque.front().key;
    auto changeAddress = changeKey.address();

    CAmount jsChange = 0;          // this is updated after each joinsplit
    int changeOutputIndex = -1;    // this is updated after each joinsplit if jsChange > 0
    bool vpubOldProcessed = false; // updated when vpub_old for taddr inputs is set in first joinsplit
    bool vpubNewProcessed = false; // updated when vpub_new for miner fee and taddr outputs is set in last joinsplit

    CAmount valueOut = 0;
    for (auto jsInput : jsInputs) {
        valueOut += jsInput.note.value();
    }
    for (auto jsOutput : jsOutputs) {
        valueOut -= jsOutput.value;
    }
    CAmount vpubOldTarget = valueOut < 0 ? -valueOut : 0;
    CAmount vpubNewTarget = valueOut > 0 ? valueOut : 0;

    // Keep track of treestate within this transaction
    boost::unordered_map<uint256, SproutMerkleTree, CCoinsKeyHasher> intermediates;
    std::vector<uint256> previousCommitments;

    while (!vpubNewProcessed) {
        // Default array entries are dummy inputs and outputs
        std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin;
        std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout;
        uint64_t vpub_old = 0;
        uint64_t vpub_new = 0;

        // Set vpub_old in the first joinsplit
        if (!vpubOldProcessed) {
            vpub_old += vpubOldTarget; // funds flowing from public pool
            vpubOldProcessed = true;
        }

        CAmount jsInputValue = 0;
        uint256 jsAnchor;

        JSDescription prevJoinSplit;

        // Keep track of previous JoinSplit and its commitments
        if (mtx.vjoinsplit.size() > 0) {
            prevJoinSplit = mtx.vjoinsplit.back();
        }

        // If there is no change, the chain has terminated so we can reset the tracked treestate.
        if (jsChange == 0 && mtx.vjoinsplit.size() > 0) {
            intermediates.clear();
            previousCommitments.clear();
        }

        //
        // Consume change as the first input of the JoinSplit.
        //
        if (jsChange > 0) {
            // Update tree state with previous joinsplit
            SproutMerkleTree tree;
            {
                LOCK(cs_coinsView);
                auto it = intermediates.find(prevJoinSplit.anchor);
                if (it != intermediates.end()) {
                    tree = it->second;
                } else if (!coinsView->GetSproutAnchorAt(prevJoinSplit.anchor, tree)) {
                    // "Could not find previous JoinSplit anchor"
                    return false;
                }
            }

            assert(changeOutputIndex != -1);
            boost::optional<SproutWitness> changeWitness;
            int n = 0;
            for (const uint256& commitment : prevJoinSplit.commitments) {
                tree.append(commitment);
                previousCommitments.push_back(commitment);
                if (!changeWitness && changeOutputIndex == n++) {
                    changeWitness = tree.witness();
                } else if (changeWitness) {
                    changeWitness.get().append(commitment);
                }
            }
            if (!changeWitness) {
                return false;
            }
            jsAnchor = tree.root();
            intermediates.insert(std::make_pair(tree.root(), tree)); // chained js are interstitial (found in between block boundaries)

            // Decrypt the change note's ciphertext to retrieve some data we need
            ZCNoteDecryption decryptor(changeKey.receiving_key());
            auto hSig = prevJoinSplit.h_sig(*sproutParams, mtx.joinSplitPubKey);
            try {
                auto plaintext = libzcash::SproutNotePlaintext::decrypt(
                    decryptor,
                    prevJoinSplit.ciphertexts[changeOutputIndex],
                    prevJoinSplit.ephemeralKey,
                    hSig,
                    (unsigned char)changeOutputIndex);

                auto note = plaintext.note(changeAddress);
                vjsin[0] = libzcash::JSInput(changeWitness.get(), note, changeKey);

                jsInputValue += plaintext.value();

                LogPrint("zrpcunsafe", "spending change (amount=%s)\n", FormatMoney(plaintext.value()));

            } catch (const std::exception& e) {
                // "Error decrypting output note of previous JoinSplit"
                return false;
            }
        }

        //
        // Consume spendable non-change notes
        //
        for (int n = (jsChange > 0) ? 1 : 0; n < ZC_NUM_JS_INPUTS && jsInputsDeque.size() > 0; n++) {
            auto jsInput = jsInputsDeque.front();
            jsInputsDeque.pop_front();

            // Add history of previous commitments to witness
            if (jsChange > 0) {
                for (const uint256& commitment : previousCommitments) {
                    jsInput.witness.append(commitment);
                }
                if (jsAnchor != jsInput.witness.root()) {
                    // "Witness for spendable note does not have same anchor as change input"
                    return false;
                }
            }

            // The jsAnchor is null if this JoinSplit is at the start of a new chain
            if (jsAnchor.IsNull()) {
                jsAnchor = jsInput.witness.root();
            }

            jsInputValue += jsInput.note.value();
            vjsin[n] = jsInput;
        }

        // Find recipient to transfer funds to
        libzcash::JSOutput recipient;
        if (jsOutputsDeque.size() > 0) {
            recipient = jsOutputsDeque.front();
            jsOutputsDeque.pop_front();
        }
        // `recipient` is now either a valid recipient, or a dummy output with value = 0

        // Reset change
        jsChange = 0;
        CAmount outAmount = recipient.value;

        // Set vpub_new in the last joinsplit (when there are no more notes to spend or zaddr outputs to satisfy)
        if (jsOutputsDeque.empty() && jsInputsDeque.empty()) {
            assert(!vpubNewProcessed);
            if (jsInputValue < vpubNewTarget) {
                // strprintf("Insufficient funds for vpub_new %s (miners fee %s, taddr outputs %s)",
                //     FormatMoney(vpubNewTarget), FormatMoney(minersFee), FormatMoney(t_outputs_total));
                return false;
            }
            outAmount += vpubNewTarget;
            vpub_new += vpubNewTarget; // funds flowing back to public pool
            vpubNewProcessed = true;
            jsChange = jsInputValue - outAmount;
            assert(jsChange >= 0);
        } else {
            // This is not the last joinsplit, so compute change and any amount still due to the recipient
            if (jsInputValue > outAmount) {
                jsChange = jsInputValue - outAmount;
            } else if (outAmount > jsInputValue) {
                // Any amount due is owed to the recipient.  Let the miners fee get paid first.
                CAmount due = outAmount - jsInputValue;
                libzcash::JSOutput recipientDue(recipient.addr, due);
                recipientDue.memo = recipient.memo;
                jsOutputsDeque.push_front(recipientDue);

                // reduce the amount being sent right now to the value of all inputs
                recipient.value = jsInputValue;
            }
        }

        // create output for recipient
        assert(ZC_NUM_JS_OUTPUTS == 2); // If this changes, the logic here will need to be adjusted
        vjsout[0] = recipient;

        // create output for any change
        if (jsChange > 0) {
            vjsout[1] = libzcash::JSOutput(changeAddress, jsChange);

            LogPrint("zrpcunsafe", "generating note for change (amount=%s)\n", FormatMoney(jsChange));
        }

        std::array<size_t, ZC_NUM_JS_INPUTS> inputMap;
        std::array<size_t, ZC_NUM_JS_OUTPUTS> outputMap;
        CreateJSDescription(vpub_old, vpub_new, vjsin, vjsout, inputMap, outputMap);

        if (jsChange > 0) {
            changeOutputIndex = -1;
            for (size_t i = 0; i < outputMap.size(); i++) {
                if (outputMap[i] == 1) {
                    changeOutputIndex = i;
                }
            }
            assert(changeOutputIndex != -1);
        }
    }
}

void TransactionBuilder::CreateJSDescription(
    uint64_t vpub_old,
    uint64_t vpub_new,
    std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin,
    std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout,
    std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
    std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap)
{
    LogPrint("zrpcunsafe", "%s: creating joinsplit at index %d (vpub_old=%s, vpub_new=%s, in[0]=%s, in[1]=%s, out[0]=%s, out[1]=%s)\n",
        mtx.vjoinsplit.size(),
        FormatMoney(vpub_old), FormatMoney(vpub_new),
        FormatMoney(vjsin[0].note.value()), FormatMoney(vjsin[1].note.value()),
        FormatMoney(vjsout[0].value), FormatMoney(vjsout[1].value));

    uint256 esk; // payment disclosure - secret

    // Generate the proof, this can take over a minute.
    JSDescription jsdesc = JSDescription::Randomized(
            mtx.fOverwintered && (mtx.nVersion >= SAPLING_TX_VERSION),
            *sproutParams,
            mtx.joinSplitPubKey,
            vjsin[0].witness.root(),
            vjsin,
            vjsout,
            inputMap,
            outputMap,
            vpub_old,
            vpub_new,
            true, //!this->testmode,
            &esk); // parameter expects pointer to esk, so pass in address

    {
        auto verifier = libzcash::ProofVerifier::Strict();
        if (!jsdesc.Verify(*sproutParams, verifier, mtx.joinSplitPubKey)) {
            throw std::runtime_error("error verifying joinsplit");
        }
    }

    mtx.vjoinsplit.push_back(jsdesc);

    // TODO: Sprout payment disclosure
}
