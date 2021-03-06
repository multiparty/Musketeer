// Copyright (c) 2016 Nikolaj Volgushev <nikolaj@bu.edu>

/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
 * A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

#include "mpc/dag_rewriter_mpc.h"
#include "ir/dummy_operator.h"
#include "ir/relation.h"
#include "ir/owner.h"
#include "base/flags.h"
#include <queue>
#include <algorithm>
#include <sstream>
#include <string>
#include <fstream>
#include <streambuf>

namespace musketeer {
namespace mpc {
    
    void DAGRewriterMPC::RewriteDAG(op_nodes& dag, StateTranslator* translator) {
        op_nodes order = op_nodes();
        TopologicalOrder(dag, &order);
        
        PropagateOwnership(order);
        
        Environment obls;
        map<string, bool> mode;
        
        set<string> inputs;
        DetermineInputs(dag, &inputs);
        
        InitEnvAndMode(obls, mode, &inputs);
        DeriveObligations(order, obls, mode, dag, translator);
        RewriteDAG(dag, obls, mode);
        PruneDAG(dag, order);

    }

    void DAGRewriterMPC::RewriteDAG(op_nodes& dag) {
        RewriteDAG(dag, NULL);
    }

    // pre-condition: l_obl or r_obl not null
    bool DAGRewriterMPC::ProcessObligation(Obligation* l_obl, Obligation* r_obl,
                                           shared_ptr<OperatorNode> cur, string l_name,
                                           string r_name, Environment& obls) {
        Obligation* obl = NULL;
        string par_name;
        Obligation* other_obl = NULL;
        string other_par_name;

        if (l_obl) {
            obl = l_obl;
            other_obl = r_obl;
            par_name = l_name;
            other_par_name = r_name; 
        }
        else {
            // r_obl is not null as per pre-condition
            obl = r_obl;
            par_name = r_name;
        }

        string cur_name = cur->get_operator()->get_output_relation()->get_name();
        
        if (obl->CanPassOperator(cur->get_operator(), other_obl)) {
            // we can "merge" obligations and so we only need to pass one of them on
            // the other obligation can be destroyed
            // delete other_obl;
            obls.push_obligation(cur_name, obl);
            return false;
        }
        else {
            // Block obligations by pushing them back to parents.
            // This also means that we need to enter MPC mode.
            LOG(INFO) << cur_name << " blocked obligation.";
            if (obl->CanAbsorb(cur->get_operator())) {
                return true;
            }
            obl->set_blocked_by(cur);
            obls.push_obligation(par_name, obl);
            if (other_obl) {
                other_obl->set_blocked_by(cur);
                obls.push_obligation(other_par_name, other_obl);
            }
            return true;
        }
    }

    bool DAGRewriterMPC::ProcessObligation(Obligation* obl, shared_ptr<OperatorNode> cur,  
                                           string par_rel_name, Environment& obls) {
        return ProcessObligation(obl, NULL, cur, par_rel_name, "", obls);
    }

    bool DAGRewriterMPC::EmitObligation(shared_ptr<OperatorNode> node, Environment& obls) {
        OperatorType op_type = node->get_operator()->get_type();
        string rel_name = node->get_operator()->get_output_relation()->get_name();
        if (op_type == AGG_OP) { // Don't forget MIN etc.
            LOG(INFO) << rel_name << " is an aggregation.";
            int num_children = node->get_children().size();
            for (int i = 0; i < num_children; ++i) {
                Obligation* obl = new Obligation(node, i);
                obls.push_obligation(rel_name, obl);
                LOG(INFO) << "Emitting obligation " << obl->get_name();
            }
            // Leaf node special case (sort of ugly)
            if (num_children == 0) {
                Obligation* obl = new Obligation(node, 0);
                obls.push_obligation(rel_name, obl);
                LOG(INFO) << "Emitting obligation " << obl->get_name();    
            }
            return false; // just emitted an obligation so we can stay in local mode
        }
        else if (op_type == JOIN_OP || op_type == UNION_OP) {
            obls.init_for(rel_name);
            return true;
        }
        else {
            obls.init_for(rel_name);
            return false;
        }
    }

    void DAGRewriterMPC::DeriveObligations(op_nodes& order, Environment& obls, 
                                           map<string, bool>& mpc_mode, op_nodes& dag, 
                                           StateTranslator* translator) {
        for (vector<shared_ptr<OperatorNode>>::iterator cur = order.begin(); cur != order.end(); ++cur) {
            // If we have a state_translator object, we should store intermediate state
            // at each step of the derivation, i.e., upon visiting each node in the dag 
            if (translator) {
                translator->StoreAsDagre(make_shared<OperatorNode>(nullptr), dag, obls, mpc_mode);
                translator->StoreAsDagre(*cur, dag, obls, mpc_mode);
            }

            Relation* rel = (*cur)->get_operator()->get_output_relation();
            LOG(INFO) << "Deriving obligations for " << rel->get_name();
            
            if (!rel->isShared()) {
                // the output relation (and consequently the input relations) 
                // is owned by only one party so we don't need mpc OR obligations
                LOG(INFO) << "Relation " << rel->get_name() << " is not shared";
                mpc_mode[rel->get_name()] = false;
                continue;    
            }
            
            vector<shared_ptr<OperatorNode>> parents = (*cur)->get_parents();
            if (parents.size() == 0) {
                LOG(INFO) << "Root rel found: " << rel->get_name();
                mpc_mode[rel->get_name()] = EmitObligation(*cur, obls);
            }
            else if (parents.size() == 1) {
                string par_name = parents[0]->get_operator()->get_output_relation()->get_name();
                LOG(INFO) << "Found parent: " << par_name;

                if (mpc_mode[par_name]) {
                    // We're already in MPC mode. No need to push obligations further.
                    mpc_mode[rel->get_name()] = true;
                    continue;
                }

                if (obls.has_obligation(par_name)) {
                    // Push or block parent obligation and update mode.
                    Obligation* par_obl = obls.pop_obligation(par_name);
                    mpc_mode[rel->get_name()] = ProcessObligation(par_obl, *cur, 
                        par_name, obls);
                }
                else {
                    // We didn't push or block obligations so check if we need to
                    // emit new obligations and update mode
                    mpc_mode[rel->get_name()] = EmitObligation(*cur, obls); 
                }            
            }
            else if (parents.size() == 2) {
                string left_name = parents[0]->get_operator()->get_output_relation()->get_name();
                string right_name = parents[1]->get_operator()->get_output_relation()->get_name();
                LOG(INFO) << "Found parents: " << left_name << " " << right_name;

                if (mpc_mode[left_name] || mpc_mode[right_name]) {
                    // We're already in MPC mode. No need to push obligations further.
                    mpc_mode[rel->get_name()] = true;
                    continue;
                }

                if (obls.has_obligation(left_name) || obls.has_obligation(right_name)) {
                    Obligation* left_obl = NULL;
                    Obligation* right_obl = NULL;

                    if (obls.has_obligation(left_name)) {
                        left_obl = obls.pop_obligation(left_name);
                    }
                    if (obls.has_obligation(right_name)) {
                        right_obl = obls.pop_obligation(right_name);
                    }

                    mpc_mode[rel->get_name()] = ProcessObligation(left_obl, right_obl,
                        *cur, left_name, right_name, obls);
                }
                else {
                    // We didn't push or block obligations so check if we need to
                    // emit new obligations and update mode
                    mpc_mode[rel->get_name()] = EmitObligation(*cur, obls);    
                }
            }
            else {
                LOG(FATAL) << "Unexpected number of parent nodes";
            }
        }
        // Store final state if we have a StateTranslator object
        if (translator) {
            translator->StoreAsDagre(make_shared<OperatorNode>(nullptr), dag, obls, mpc_mode);
        }
    }

    void DAGRewriterMPC::InitEnvAndMode(Environment& obls, map<string, bool>& mpc_mode,
                                        set<string>* inputs) {
        for (set<string>::iterator i = inputs->begin(); i != inputs->end(); ++i) {
            mpc_mode[(*i)] = false;
            obls.init_for((*i));
        }
    }

    shared_ptr<OperatorNode> DAGRewriterMPC::InsertNode(shared_ptr<OperatorNode> at_node,
                                                        shared_ptr<OperatorNode> child_node, 
                                                        shared_ptr<OperatorNode> new_node) {        
        op_nodes at_children = at_node->get_children();
        OperatorInterface* at_op = at_node->get_operator();
        Relation* at_rel = at_op->get_output_relation();

        OperatorInterface* new_op = new_node->get_operator();
        Relation* new_rel = new_op->get_output_relation();

        LOG(INFO) << "Inserting obligation node: " << new_rel->get_name();

        vector<Relation*> new_rels;
        new_rels.push_back(at_rel);

        new_op->set_relations(new_rels);
        new_op->update_columns();

        if (child_node) {
            LOG(INFO) << "Child node found";
            at_children.erase(remove(at_children.begin(), at_children.end(), child_node), 
                              at_children.end());
            at_children.push_back(new_node);
            at_node->set_children(at_children);

            op_nodes wrapper;
            wrapper.push_back(child_node);
            new_node->set_children(wrapper);

            op_nodes child_parents = child_node->get_parents();
            child_parents.erase(remove(child_parents.begin(), child_parents.end(), at_node), 
                                child_parents.end());
            child_parents.push_back(new_node);
                
            // Update the relations attached to the operator on the child node
            OperatorInterface* child_op = child_node->get_operator();
            vector<Relation*> child_rels = child_op->get_relations();
            vector<Relation*> updated_rels;

            for (vector<Relation*>::iterator r = child_rels.begin(); 
                 r != child_rels.end(); ++r) {
                if (*r != at_rel) {
                    updated_rels.push_back(*r);
                }
                else {
                    updated_rels.push_back(new_rel);    
                }
            }
            child_op->set_relations(updated_rels);
            child_op->update_columns();    
        }

        return new_node;
    }

    void DAGRewriterMPC::RewriteDAG(op_nodes& dag, Environment& obls, 
                                    map<string, bool>& mpc_mode) {
        set<shared_ptr<OperatorNode>> visited;
        queue<shared_ptr<OperatorNode>> to_visit;
        for (op_nodes::iterator it = dag.begin(); it != dag.end(); ++it) {
            to_visit.push(*it);
            visited.insert(*it);
        }

        while (!to_visit.empty()) {
            shared_ptr<OperatorNode> cur_node = to_visit.front();
            string rel_name = cur_node->get_operator()->get_output_relation()->get_name();
            LOG(INFO) << "Visiting node " << rel_name;
            to_visit.pop();

            if (!cur_node->IsLeaf()) {
                op_nodes children = cur_node->get_children();
                for (op_nodes::iterator it = children.begin(); it != children.end();
                     ++it) {
                    if (visited.insert(*it).second) {
                        to_visit.push(*it);
                    }
                }
            }

            // Note that the conditions below are mutually exclusive (by design)
            if (mpc_mode[rel_name]) {
                LOG(INFO) << "Replacing with MPC operator for " << rel_name; 
                OperatorInterface* mpc_op = cur_node->get_operator()->toMPC();
                cur_node->replace_operator(mpc_op);   
            }
            else if (obls.has_obligation(rel_name)) {
                LOG(INFO) << "Inserting obligation for " << rel_name;
                Obligation* obl = obls.pop_obligation(rel_name);
                OperatorInterface* obl_op = obl->get_operator();
                shared_ptr<OperatorNode> blocker = obl->get_blocked_by();
                shared_ptr<OperatorNode> inserted = 
                    InsertNode(cur_node, blocker, make_shared<OperatorNode>(obl_op));
            }
            else {
                // LOG(ERROR) << ""
            }
        }        
    }

    void DAGRewriterMPC::PropagateOwnership(op_nodes& dag) {
        // TODO(nikolaj): this is pretty hacky
        map<string, set<Owner*>> owner_lookup;
        for (vector<shared_ptr<OperatorNode>>::iterator i = dag.begin(); i != dag.end(); ++i) {
            OperatorInterface* op = (*i)->get_operator();
            Relation* out_rel = op->get_output_relation();
            vector<Relation*> in_rels = op->get_relations();
            
            for (vector<Relation*>::iterator r = in_rels.begin(); r != in_rels.end(); ++r) {
                set<Owner*> owners_on_inrel = (*r)->get_owners();
                (*r)->add_owners(owner_lookup[(*r)->get_name()]);
                owner_lookup[(*r)->get_name()].insert(owners_on_inrel.begin(), owners_on_inrel.end());
                set<Owner*> in_owners = owner_lookup[(*r)->get_name()];
                owner_lookup[out_rel->get_name()].insert(in_owners.begin(), in_owners.end());
            }
            out_rel->add_owners(owner_lookup[out_rel->get_name()]);
            LOG(INFO) << "Propagating ownership for " << out_rel->get_name();

            string owner_str = "";
            set<Owner*> temp = out_rel->get_owners();
            for (set<Owner*>::iterator i = temp.begin(); 
                 i != temp.end(); ++i) {
                owner_str += (*i)->get_name() + " ";
            }
            LOG(INFO) << "Rel " << out_rel->get_name() << " has owners " << owner_str;  
        }
    }

    void DAGRewriterMPC::PruneDAG(op_nodes& roots, op_nodes& dag) {
        LOG(INFO) << "Pruning DAG";
        set<shared_ptr<OperatorNode>> bad_nodes;

        // Record all "bad" nodes we need to remove from graph
        for (vector<shared_ptr<OperatorNode>>::iterator i = dag.begin(); i != dag.end(); ++i) {
            Relation* outrel = (*i)->get_operator()->get_output_relation();
            if (!(outrel->has_owner(FLAGS_data_owner_id))) {
                LOG(INFO) << "Relation owned by someone else: " << outrel->get_name();
                bad_nodes.insert(*i);
                // roots.erase(remove(roots.begin(), roots.end(), *i), roots.end());
                OperatorInterface* current_op = (*i)->get_operator(); 
                Relation* rel = current_op->get_output_relation();
                LOG(INFO) << "Found node we can remove: " << rel->get_name();
                vector<Relation*> rels;
                rels.push_back(rel);
                OperatorInterface* dummy_op = new DummyOperator(current_op->get_output_path(), rels, rel);
                (*i)->replace_operator(dummy_op);
            }
        }

        for (vector<shared_ptr<OperatorNode>>::iterator i = dag.begin(); i != dag.end(); ++i) {
            LOG(INFO) << "Pruning node: " << (*i)->get_operator()->get_output_relation()->get_name();

            op_nodes children = (*i)->get_children();
            op_nodes new_children;
            op_nodes parents = (*i)->get_parents();
            
            for (vector<shared_ptr<OperatorNode>>::iterator j = children.begin(); j != children.end(); ++j) {
                if (find(bad_nodes.begin(), bad_nodes.end(), (*j)) == bad_nodes.end()) {
                    new_children.push_back(*j);
                }
            }

            for (vector<shared_ptr<OperatorNode>>::iterator j = parents.begin(); j != parents.end(); ++j) {
                if (find(bad_nodes.begin(), bad_nodes.end(), (*j)) != bad_nodes.end()) {
                    OperatorInterface* current_op = (*j)->get_operator(); 
                    Relation* rel = current_op->get_output_relation();
                    LOG(INFO) << "Found node we can remove: " << rel->get_name();
                    vector<Relation*> rels;
                    rels.push_back(rel);
                    OperatorInterface* dummy_op = new DummyOperator(current_op->get_output_path(), rels, rel);
                    (*j)->replace_operator(dummy_op);
                }
            }

            (*i)->set_children(new_children);
        }
    }

} // namespace mpc
} // namespace musketeer
