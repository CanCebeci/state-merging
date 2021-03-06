/*
 * substitutionMap.h
 *
 */

#ifndef SUBSTITUTIONMAP_H_
#define SUBSTITUTIONMAP_H_

#include "../AST/AST.h"
#include "../STPManager/STPManager.h"
#include "../AST/NodeFactory/SimplifyingNodeFactory.h"
#include "VariablesInExpression.h"

namespace BEEV {

class Simplifier;
class ArrayTransformer;

const bool debug_substn = false;

class SubstitutionMap {

	ASTNodeMap * SolverMap;
	Simplifier *simp;
	STPMgr* bm;
	ASTNode ASTTrue, ASTFalse, ASTUndefined;
	NodeFactory *nf;

	// These are used to avoid substituting {x = f(y,z), z = f(x)}
	typedef hash_map<ASTNode, Symbols*,ASTNode::ASTNodeHasher> DependsType;
	DependsType dependsOn; // The lhs depends on the variables in the rhs.
	ASTNodeSet rhs; // All the rhs that have been seeen.
	set<ASTNodeSet*> rhsAlreadyAdded;
	VariablesInExpression::SymbolPtrSet rhs_visited; // the rhs contains all the variables in here already.
	HASHSET<int> alreadyVisited;

	int loopCount;

	void buildDepends(const ASTNode& n0, const ASTNode& n1);
	void loops_helper(const set<ASTNode>& varsToCheck, set<ASTNode>& visited);
	bool loops(const ASTNode& n0, const ASTNode& n1);

	int substitutionsLastApplied;
public:

	bool hasUnappliedSubstitutions()
	{
	  return (substitutionsLastApplied != SolverMap->size());
	}

	// When the substitutionMap has been applied globally, then,
	// these are no longer needed.
	void haveAppliedSubstitutionMap()
	{
		dependsOn.clear();
		rhs.clear();
		rhs_visited.clear();
		rhsAlreadyAdded.clear();
		substitutionsLastApplied = SolverMap->size();
	}

	VariablesInExpression vars;

	SubstitutionMap(Simplifier *_simp, STPMgr* _bm)
	{
		simp = _simp;
		bm = _bm;

		ASTTrue  = bm->CreateNode(TRUE);
	      ASTFalse = bm->CreateNode(FALSE);
	      ASTUndefined = bm->CreateNode(UNDEFINED);

		SolverMap = new ASTNodeMap(INITIAL_TABLE_SIZE);
		loopCount = 0;
		substitutionsLastApplied =0;
	        nf = new SimplifyingNodeFactory (*bm->hashingNodeFactory, *bm);
	}

	void clear()
	{
		SolverMap->clear();
		haveAppliedSubstitutionMap();
		alreadyVisited.clear();
	}

	virtual ~SubstitutionMap();

	//check the solver map for 'key'. If key is present, then return the
	//value by reference in the argument 'output'
	bool CheckSubstitutionMap(const ASTNode& key, ASTNode& output) {
		ASTNodeMap::iterator it = SolverMap->find(key);
		if (it != SolverMap->end()) {
			output = it->second;
			return true;
		}
		return false;
	}

	//update solvermap with (key,value) pair
	bool UpdateSolverMap(const ASTNode& key, const ASTNode& value) {
		ASTNode var = (BVEXTRACT == key.GetKind()) ? key[0] : key;

		if (var.GetKind() == SYMBOL && loops(var,value))
			return false;


		if (!CheckSubstitutionMap(var) && key != value) {
			//cerr << "from" << key << "to" <<value;
			buildDepends(key,value);
			(*SolverMap)[key] = value;
			return true;
		}
		return false;
	} //end of UpdateSolverMap()

	 ASTNodeMap * Return_SolverMap() {
		return SolverMap;
	} // End of SolverMap()

	bool CheckSubstitutionMap(const ASTNode& key) {
		if (SolverMap->find(key) != SolverMap->end())
			return true;
		else
			return false;
	}

	// It's depressingly expensive to perform all of the loop checks etc.
	// If you use this function you are promising:
	// 1) That UpdateSubstitutionMap(e0,e1) would have returned true.
	// 2) That all of the substitutions will be written in fully before other code
        bool UpdateSubstitutionMapFewChecks(const ASTNode& e0, const ASTNode& e1)
        {
          assert(e0.GetKind() == SYMBOL);
          assert (!CheckSubstitutionMap(e0));
          (*SolverMap)[e0] = e1;
          return true;
        }

	// The substitutionMap will be updated, given x <-> f(w,z,y), iff,
	// 1) x doesn't appear in the rhs.
	// 2) x hasn't already been stored in the substitution map.
	// 3) none of the variables in the transitive closure of the rhs depend on x.
	bool UpdateSubstitutionMap(const ASTNode& e0, const ASTNode& e1) {
		int i = TermOrder(e0, e1);
		if (0 == i)
			return false;

		assert(e0 != e1);
		assert(e0.GetValueWidth() == e1.GetValueWidth());
		assert(e0.GetIndexWidth() == e1.GetIndexWidth());

		if (e0.GetKind() == SYMBOL)
		{
			if (CheckSubstitutionMap(e0))
				return false; // already in the map.

			if (loops(e0,e1))
				return false; // loops.
		}

		if (e1.GetKind() == SYMBOL)
		{
			if (CheckSubstitutionMap(e1))
				return false; // already in the map.


			if (loops(e1,e0))
				return false; // loops
		}

		//e0 is of the form READ(Arr,const), and e1 is const, or
		//e0 is of the form var, and e1 is a function.
		if (1 == i && !CheckSubstitutionMap(e0)) {
			buildDepends(e0,e1);
			(*SolverMap)[e0] = e1;
			return true;
		}

		//e1 is of the form READ(Arr,const), and e0 is const, or
		//e1 is of the form var, and e0 is const
		if (-1 == i && !CheckSubstitutionMap(e1)) {
			buildDepends(e1,e0);
			(*SolverMap)[e1] = e0;
			return true;
		}

		return false;
	}

	ASTNode CreateSubstitutionMap(const ASTNode& a,   ArrayTransformer*at);

	ASTNode applySubstitutionMap(const ASTNode& n);

	ASTNode applySubstitutionMapUntilArrays(const ASTNode& n);

	// Replace any nodes in "n" that exist in the fromTo map.
	// NB the fromTo map is changed.
	static ASTNode replace(const ASTNode& n, ASTNodeMap& fromTo, ASTNodeMap& cache, NodeFactory *nf);
	static ASTNode replace(const ASTNode& n, ASTNodeMap& fromTo, ASTNodeMap& cache, NodeFactory *nf, bool stopAtArrays);


};

}
;
#endif /* SUBSTITUTIONMAP_H_ */
