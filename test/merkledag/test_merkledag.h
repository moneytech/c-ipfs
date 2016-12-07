#include "ipfs/merkledag/merkledag.h"
#include "ipfs/node/node.h"
#include "../test_helper.h"

int test_merkledag_get_data() {
	int retVal = 0;

	// create a fresh repo
	retVal = drop_and_build_repository("/tmp/.ipfs");
	if (retVal == 0)
		return 0;

	// open the fs repo
	struct RepoConfig* repo_config = NULL;
	struct FSRepo* fs_repo;
	const char* path = "/tmp/.ipfs";

	// create the struct
	retVal = ipfs_repo_fsrepo_new((char*)path, repo_config, &fs_repo);
	if (retVal == 0)
		return 0;

	// open the repository and read the config file
	retVal = ipfs_repo_fsrepo_open(fs_repo);
	if (retVal == 0) {
		ipfs_repo_fsrepo_free(fs_repo);
		return 0;
	}

	// create data for node
	size_t binary_data_size = 256;
	unsigned char binary_data[binary_data_size];
	for(int i = 0; i < binary_data_size; i++) {
		binary_data[i] = i;
	}

	// create a node
	struct Node* node1 = N_Create_From_Data(binary_data, binary_data_size);

	retVal = ipfs_merkledag_add(node1, fs_repo);
	if (retVal == 0) {
		Node_Delete(node1);
		ipfs_repo_fsrepo_free(fs_repo);
		return 0;
	}

	// now retrieve it
	struct Node* results_node;
	retVal = ipfs_merkledag_get(node1->cached, &results_node, fs_repo);
	if (retVal == 0) {
		Node_Delete(node1);
		Node_Delete(results_node);
		ipfs_repo_fsrepo_free(fs_repo);
		return 0;
	}

	if (results_node->data_size != 256) {
		Node_Delete(node1);
		Node_Delete(results_node);
		ipfs_repo_fsrepo_free(fs_repo);
		return 0;
	}

	// the data should be the same
	for(int i = 0; i < results_node->data_size; i++) {
		if (results_node->data[i] != node1->data[i]) {
			Node_Delete(node1);
			Node_Delete(results_node);
			ipfs_repo_fsrepo_free(fs_repo);
			return 0;
		}
	}

	Node_Delete(node1);
	Node_Delete(results_node);
	ipfs_repo_fsrepo_free(fs_repo);

	return retVal;
}

int test_merkledag_add_data() {
	int retVal = 0;

	// create a fresh repo
	retVal = drop_and_build_repository("/tmp/.ipfs");
	if (retVal == 0)
		return 0;

	// open the fs repo
	struct RepoConfig* repo_config = NULL;
	struct FSRepo* fs_repo;
	const char* path = "/tmp/.ipfs";

	// create the struct
	retVal = ipfs_repo_fsrepo_new((char*)path, repo_config, &fs_repo);
	if (retVal == 0)
		return 0;

	// open the repository and read the config file
	retVal = ipfs_repo_fsrepo_open(fs_repo);
	if (retVal == 0) {
		ipfs_repo_fsrepo_free(fs_repo);
		return 0;
	}

	// get the size of the database
	int start_file_size = os_utils_file_size("/tmp/.ipfs/datastore/data.mdb");

	// create data for node
	size_t binary_data_size = 256;
	unsigned char binary_data[binary_data_size];
	for(int i = 0; i < binary_data_size; i++) {
		binary_data[i] = i;
	}

	// create a node
	struct Node* node1 = N_Create_From_Data(binary_data, binary_data_size);

	retVal = ipfs_merkledag_add(node1, fs_repo);
	if (retVal == 0) {
		Node_Delete(node1);
		return 0;
	}

	// make sure everything is correct
	if (node1->cached == NULL)
		return 0;

	int first_add_size = os_utils_file_size("/tmp/.ipfs/datastore/data.mdb");
	if (first_add_size == start_file_size) { // uh oh, database should have increased in size
		Node_Delete(node1);
		return 0;
	}

	// adding the same binary again should do nothing (the hash should be the same)
	struct Node* node2 = N_Create_From_Data(binary_data, binary_data_size);
	retVal = ipfs_merkledag_add(node2, fs_repo);
	if (retVal == 0) {
		Node_Delete(node1);
		Node_Delete(node2);
		return 0;
	}

	// make sure everything is correct
	if (node2->cached == NULL) {
		Node_Delete(node1);
		Node_Delete(node2);
		return 0;
	}
	for(int i = 0; i < node1->cached->hash_length; i++) {
		if (node1->cached->hash[i] != node2->cached->hash[i]) {
			printf("hash of node1 does not match node2 at position %d\n", i);
			Node_Delete(node1);
			Node_Delete(node2);
			return 0;
		}
	}

	int second_add_size = os_utils_file_size("/tmp/.ipfs/datastore/data.mdb");
	if (first_add_size != second_add_size) { // uh oh, the database shouldn't have changed size
		printf("looks as if a new record was added when it shouldn't have. Old file size: %d, new file size: %d\n", first_add_size, second_add_size);
		Node_Delete(node1);
		Node_Delete(node2);
		return 0;
	}

	// now change 1 byte, which should change the hash
	binary_data[10] = 0;
	// create a node
	struct Node* node3 = N_Create_From_Data(binary_data, binary_data_size);

	retVal = ipfs_merkledag_add(node3, fs_repo);
	if (retVal == 0) {
		Node_Delete(node1);
		Node_Delete(node2);
		Node_Delete(node3);
		return 0;
	}

	// make sure everything is correct
	if (node3->cached == NULL) {
		Node_Delete(node1);
		Node_Delete(node2);
		Node_Delete(node3);
		return 0;
	}

	Node_Delete(node1);
	Node_Delete(node2);
	Node_Delete(node3);
	int third_add_size = os_utils_file_size("/tmp/.ipfs/datastore/data.mdb");
	if (third_add_size == second_add_size || third_add_size < second_add_size) {// uh oh, it didn't add it
		printf("Node 3 should have been added, but the file size did not change from %d.\n", third_add_size);
		return 0;
	}

	ipfs_repo_fsrepo_free(fs_repo);

	return 1;
}
