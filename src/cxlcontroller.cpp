//
// Created by victoryang00 on 1/14/23.
//

#include "cxlcontroller.h"

void CXLController::construct_topo(std::string newick_tree) {
    char newickTreeC[newick_tree.size() + 1];
    int size, i = 0;
    size = newick_tree.size();
    strcpy(newickTreeC, newick_tree.c_str());
    construct_one(newickTreeC, i, size, end_points[0]);
    counter = new CXLCounter(num_switches);
}
void CXLController::insert_end_point(CXLEndPoint *end_point) { this->end_points.emplace_back(end_point); }
void CXLController::construct_one(char *newick_tree, int &index, int end, CXLEndPoint &node) {
    if (newick_tree[index] != '(') {
        LOG(ERROR) << "topology input error\n";
        throw;
    }
    // left child name
    node.lChild = new TreeNode;
    node.lChild->father = &node;
    node.lChild->cFlag = false;

    if (newick_tree[++index] == '(') {
        construct_one(newick_tree, index, end, *(node.lChild));
        while (newick_tree[index] != ':')
            index++; // pass the number
    } else {
        while (newick_tree[index] != ':') // set name, now index to :
            node.lChild->name += newick_tree[index++];
    }

    // left dist
    index++;
    char *startC = newick_tree + index;
    while (newick_tree[index] != ',')
        index++;
    // endC=newick_tree+(index-1);
    node.lDist = strtod(startC, nullptr);
    node.lChild->fDist = node.lDist;
    // now index to ',', and need to process right
    node.rChild = new TreeNode;
    node.rChild->father = &node;
    node.rChild->cFlag = true;
    treeSize++;

    // right child name
    if (newick_tree[++index] == '(') {
        parseOne(newick_tree, index, end, *(node.rChild), mod);
        while (newick_tree[index] != ':')
            index++;
    } else {
        while (newick_tree[index] != ':') // set name, now index to :
            node.rChild->name += newick_tree[index++];
    }
    // right dist
    index++;
    startC = newick_tree + index;
    while (newick_tree[index] != ')')
        index++;
    // endC=newick_tree+(index-1);
    node.rDist = strtod(startC, NULL);
    node.rChild->fDist = node.rDist;
}

CXLController::CXLController(Policy p) { this->policy = p; }
double CXLController::calculate_latency(double weight, struct Elem *elem) { return 0; }
double CXLController::calculate_bandwidth(double weight, struct Elem *elem) { return 0; }
void CXLController::print() {}
