#include <boost/test/unit_test.hpp>
#include <test/data/ethspv_valid.json.h>
#include <test/data/ethspv_invalid.json.h>

#include "uint256.h"
#include "util.h"
#include "utilstrencodings.h"
#include "ethereum/ethereum.h"
#include "ethereum/Common.h"
#include "ethereum/RLP.h"
#include "script/interpreter.h"
#include "script/standard.h"
#include "policy/policy.h"
#include "services/asset.h"
#include <univalue.h>

extern UniValue read_json(const std::string& jsondata);


BOOST_AUTO_TEST_SUITE(ethereum_tests)
BOOST_AUTO_TEST_CASE(ethereum_parseabidata)
{
    CAmount outputAmount = 0;
    uint32_t nAsset = 0;
    const std::vector<unsigned char> &expectedMethodHash = ParseHex("a82e762b");
    const std::vector<unsigned char> &rlpBytes = ParseHex("a82e762b00000000000000000000000000000000000000000000000000000000773594009591c8dc");
    CWitnessAddress address;
    BOOST_CHECK(parseEthMethodInputData(expectedMethodHash, rlpBytes, outputAmount, nAsset, address));
    BOOST_CHECK_EQUAL(outputAmount, 20*COIN);
    BOOST_CHECK_EQUAL(nAsset, 2509359324);
}

BOOST_AUTO_TEST_CASE(ethspv_valid)
{
    // Read tests from test/data/ethspv_valid.json
    // Format is an array of arrays
    // Inner arrays are either [ "comment" ]
    // [[spv_root, spv_parent_node, spv_value, spv_path]]

    UniValue tests = read_json(std::string(json_tests::ethspv_valid, json_tests::ethspv_valid + sizeof(json_tests::ethspv_valid)));

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        std::string strTest = test.write();
        if (test.size() != 4) {
				// ignore comments
				continue;
		} else {
        if ( !test[0].isStr() || !test[1].isStr() || !test[2].isStr() || !test[3].isStr()) {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }

      	std::string spv_tx_root = test[0].get_str();
			  std::string spv_parent_nodes = test[1].get_str();
			  std::string spv_value = test[2].get_str();
			  std::string spv_path = test[3].get_str();

        const std::vector<unsigned char> &vchTxRoot = ParseHex(spv_tx_root);
        dev::RLP rlpTxRoot(&vchTxRoot);
        const std::vector<unsigned char> &vchParentNodes = ParseHex(spv_parent_nodes);
        dev::RLP rlpParentNodes(&vchParentNodes);
        const std::vector<unsigned char> &vchValue = ParseHex(spv_value);
        dev::RLP rlpValue(&vchValue);
        const std::vector<unsigned char> &vchPath = ParseHex(spv_path);
        BOOST_CHECK(VerifyProof(&vchPath, rlpValue, rlpParentNodes, rlpTxRoot));
        }
    }
}

BOOST_AUTO_TEST_CASE(ethspv_invalid)
{
    // Read tests from test/data/ethspv_invalid.json
    // Format is an array of arrays
    // Inner arrays are either [ "comment" ]
    // [[spv_root, spv_parent_node, spv_value, spv_path]]

    UniValue tests = read_json(std::string(json_tests::ethspv_invalid, json_tests::ethspv_invalid + sizeof(json_tests::ethspv_invalid)));

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        std::string strTest = test.write();
        if (test.size() != 4) {
				// ignore comments
				continue;
		    } else {
            if ( !test[0].isStr() || !test[1].isStr() || !test[2].isStr() || !test[3].isStr()) {
                BOOST_ERROR("Bad test: " << strTest);
                continue;
            }
			      std::string spv_tx_root = test[0].get_str();
			      std::string spv_parent_nodes = test[1].get_str();
			      std::string spv_value = test[2].get_str();
			      std::string spv_path = test[3].get_str();

            const std::vector<unsigned char> &vchTxRoot = ParseHex(spv_tx_root);
            dev::RLP rlpTxRoot(&vchTxRoot);
            const std::vector<unsigned char> &vchParentNodes = ParseHex(spv_parent_nodes);
            dev::RLP rlpParentNodes(&vchParentNodes);
            const std::vector<unsigned char> &vchValue = ParseHex(spv_value);
            dev::RLP rlpValue(&vchValue);
            const std::vector<unsigned char> &vchPath = ParseHex(spv_path);
            BOOST_CHECK(!VerifyProof(&vchPath, rlpValue, rlpParentNodes, rlpTxRoot));
        }
    }
}
BOOST_AUTO_TEST_SUITE_END()
