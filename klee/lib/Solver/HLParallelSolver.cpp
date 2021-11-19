#include "klee/Solver.h"

#include "klee/Expr.h"
#include "klee/Constraints.h"
#include "klee/SolverImpl.h"
#include "klee/util/ExprHashMap.h"
#include "klee/util/ExprVisitor.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/Assignment.h"

#include <pthread.h>
#include <vector>
#include <deque>

#define SUBQUERY_CONDITIONS_MAX 8

//#define __DISABLE_MAIN_SOLVER
//#define __DISABLE_SUBSOLVERS

using namespace klee;

namespace klee {

#if 0
class HLParallelSolver : public SolverImpl {
private:
  Solver *solver;

public:
  HLParallelSolver(Solver *_solver, unsigned subsolversCount);
  ~HLParallelSolver();

  bool computeTruth(const Query &query, bool &isValid);
  bool computeValidity(const Query &query, Solver::Validity &validity);
  bool computeValue(const Query &query, ref<Expr> &value);
  bool computeInitialValues(const Query &query,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution);

  void cancelPendingJobs() { assert(0); }

protected:
  static void *mainSolverThread(void *arg);
  static void *subqueryGeneratorThread(void *arg);
  static void *subsolverThread(void *arg);

  enum QueryType {
    TruthQuery, ValidityQuery, ValueQuery, InitialValuesQuery, ExitRequest
  };

  struct QueryInfo {
    // Query description
    QueryType type;
    const ConstraintManager constraints;
    const Query query;
    const std::vector<const Array*> objects;

    // Query results
    bool solved;
    bool isValid;
    Solver::Validity validity;
    std::vector< std::vector<unsigned char> > initialValues;
    ref<Expr> value;

    bool mainSolverActive;
    bool mainSolverDone;

    int  subsolversActive;
    bool subsolversDone;

    std::vector<ref<Expr> > subqueryConditions;
    uint64_t subqueryIndex;

    bool subqueriesAreComplete;
    bool seenInvalidSubquery;
    bool seenSatSubquery;

    QueryInfo(QueryType _type)
        : type(_type), query(constraints, 0),
          solved(false), mainSolverActive(false), mainSolverDone(false),
          subsolversActive(0), subsolversDone(false),
          subqueryIndex(ULLONG_MAX) {
      assert(type == ExitRequest);
    }

    QueryInfo(QueryType _type, const Query &_query,
              const std::vector<const Array*> &_objects =
                                   std::vector<const Array*>())
        : type(_type), constraints(_query.constraints),
          query(constraints, _query.expr), objects(_objects),
          solved(false), mainSolverActive(false), mainSolverDone(false),
          subsolversActive(0), subsolversDone(false),
          subqueryIndex(ULLONG_MAX), subqueriesAreComplete(true),
          seenInvalidSubquery(false), seenSatSubquery(false) {}


    bool isDone() { return solved || (mainSolverDone && subsolversDone &&
                                      !subsolversActive); }
    bool isActive() { return mainSolverActive || subsolversActive; }

    void gatherSubqueryConditionsFor(const ref<Expr>& expr);
  };

  std::deque<QueryInfo*> queriesInfo;

  std::vector<pthread_t> threads;

  pthread_mutex_t mutex;
  pthread_cond_t queryReadyCond;
  pthread_cond_t subqueryReadyCond;
  pthread_cond_t queryDoneCond;
};

#define _CHECKED(smt) \
  do { \
    int res = smt; \
    assert(res == 0); \
  } while (0)

HLParallelSolver::HLParallelSolver(Solver *_solver, unsigned subsolversCount)
    : solver(_solver) {
  if (subsolversCount == 0)
    subsolversCount = 2;

  _CHECKED(pthread_mutex_init(&mutex, NULL));
  _CHECKED(pthread_cond_init(&queryReadyCond, NULL));
  _CHECKED(pthread_cond_init(&subqueryReadyCond, NULL));
  _CHECKED(pthread_cond_init(&queryDoneCond, NULL));

  threads.resize(subsolversCount + 2);
  //threads.resize(2);
  _CHECKED(pthread_create(&threads[0], NULL, mainSolverThread, this));
  _CHECKED(pthread_create(&threads[1], NULL, subqueryGeneratorThread, this));
  for (unsigned i = 0; i < subsolversCount; ++i)
    _CHECKED(pthread_create(&threads[i+2], NULL, subsolverThread, this));
}

HLParallelSolver::~HLParallelSolver() {
  pthread_mutex_lock(&mutex);
  QueryInfo *queryInfo = new QueryInfo(ExitRequest);
  queriesInfo.push_back(queryInfo);
  pthread_cond_broadcast(&queryReadyCond);
  pthread_mutex_unlock(&mutex);

  for (unsigned i = 0; i < threads.size(); ++i)
    pthread_join(threads[i], NULL);

  queryInfo->mainSolverDone = true;
  queryInfo->subsolversDone = true;

  while (!queriesInfo.empty()) {
    assert(!queriesInfo.front()->isActive());
    assert(queriesInfo.front()->isDone());
    delete queriesInfo.front();
    queriesInfo.pop_front();
  }

  _CHECKED(pthread_cond_destroy(&queryDoneCond));
  _CHECKED(pthread_cond_destroy(&subqueryReadyCond));
  _CHECKED(pthread_cond_destroy(&queryReadyCond));
  _CHECKED(pthread_mutex_destroy(&mutex));
}

void *HLParallelSolver::mainSolverThread(void *arg) {
  HLParallelSolver *pSolver = static_cast<HLParallelSolver*>(arg);

  bool solved;
  ref<Expr> value;
  bool hasSolution;
  bool isValid;
  Solver::Validity validity;
  std::vector< std::vector<unsigned char> > initialValues;

  pthread_mutex_lock(&pSolver->mutex);
  while (true) {
    while(pSolver->queriesInfo.empty() || pSolver->queriesInfo.back()->solved
                        || pSolver->queriesInfo.back()->mainSolverDone)
      pthread_cond_wait(&pSolver->queryReadyCond, &pSolver->mutex);

    QueryInfo *queryInfo = pSolver->queriesInfo.back();
    if (queryInfo->type == ExitRequest)
      break;

    assert(!queryInfo->mainSolverActive);
    assert(!queryInfo->mainSolverDone);
    queryInfo->mainSolverActive = true;
    solved = false;

#ifdef __DISABLE_MAIN_SOLVER
#warning Main solver is disabled!
    // Wait for generator thread to finish
    while (queryInfo->subqueryIndex == ULLONG_MAX) {
      pthread_mutex_unlock(&pSolver->mutex);
      pthread_yield();
      pthread_mutex_lock(&pSolver->mutex);
    }
    // If generator produced subqueries, mark ourselfs as done
    if (queryInfo->subqueryIndex != ULLONG_MAX-1) {
      // XXX
      queryInfo->mainSolverActive = false;
      if (!queryInfo->isDone()) {
        queryInfo->mainSolverDone = true;
        if (queryInfo->isDone()) {
          pthread_cond_signal(&pSolver->queryDoneCond);
        }
      }
      continue;
    }
    // Otherwise, we have to solve the query anyway
#endif

    pthread_mutex_unlock(&pSolver->mutex);

    switch (queryInfo->type) {
    case ValidityQuery:
      solved = pSolver->solver->impl->computeValidity(
          queryInfo->query, validity);
      break;

    case TruthQuery:
      solved = pSolver->solver->impl->computeTruth(
          queryInfo->query, isValid);
      break;

    case ValueQuery:
      solved = pSolver->solver->impl->computeValue(
          queryInfo->query, value);
      break;

    case InitialValuesQuery:
      solved = pSolver->solver->impl->computeInitialValues(
          queryInfo->query.asOneExpr(), queryInfo->objects,
          initialValues, hasSolution);
      break;

    case ExitRequest:
      assert(false);
      break;
    }

    pthread_mutex_lock(&pSolver->mutex);
    queryInfo->mainSolverActive = false;

    if (!queryInfo->isDone()) {
      queryInfo->mainSolverDone = true;
      // The solver is still interested in our results for this query

      switch (queryInfo->type) {
      case ValidityQuery: queryInfo->validity = validity; break;
      case TruthQuery: queryInfo->isValid = isValid; break;
      case ValueQuery: queryInfo->value = value; break;
      case InitialValuesQuery:
        queryInfo->initialValues.swap(initialValues);
        queryInfo->isValid = !hasSolution;
        break;
      case ExitRequest: assert(false); break;
      }

      queryInfo->solved = solved;

      if (queryInfo->isDone()) {
        pSolver->solver->impl->cancelPendingJobs();
        pthread_cond_signal(&pSolver->queryDoneCond);
      }
    }
  }
  pthread_mutex_unlock(&pSolver->mutex);

  return NULL;
}

class SubqueryBuilderVisitor : public ExprVisitor {
private:
  const std::vector<ref<Expr> > &conditions;
  const uint64_t index;

public:
  SubqueryBuilderVisitor(const std::vector<ref<Expr> > &_conditions,
                         uint64_t _index)
    : conditions(_conditions), index(_index) {}

  Action _visitExpr(const Expr &e) {
    if (e.getWidth() == Expr::Bool) {
      for (unsigned int i = 0; i < conditions.size(); ++i) {
        if (conditions[i] == ref<Expr>(const_cast<Expr*>(&e))) {
          return Action::changeTo(ConstantExpr::create(
              (index & (1ul<<i)) ? 1 : 0, 1));
        }
      }
    }
    return Action::doChildren();
  }

  Action visitExpr(const Expr &e) { return _visitExpr(e); }
  Action visitExprPost(const Expr &e) { return _visitExpr(e); }

};

void HLParallelSolver::QueryInfo::gatherSubqueryConditionsFor(
                                          const ref<Expr>& expr) {
  if (subqueryConditions.size() >= SUBQUERY_CONDITIONS_MAX)
    return;
  std::vector< ref<Expr> > stack;
  ExprHashSet visited;

  {
    // just in case, if we query one of the merge conditions...
    ConstraintManager::merge_conditions_ty::iterator it =
        constraints.mergeConditions.find(expr);
    if (it != constraints.mergeConditions.end()) {
      subqueryConditions.push_back(*it);
      if (subqueryConditions.size() >= SUBQUERY_CONDITIONS_MAX)
        return;
    }
  }

  if (!isa<ConstantExpr>(expr)) {
    visited.insert(expr);
    stack.push_back(expr);
  }

  while (!stack.empty()) {
    ref<Expr> top = stack.back();
    stack.pop_back();

    // Range analysis could help when the top is boolean (comparison
    // expressions and logical operations) or for a first child of SelectExpr.
    bool couldBeHelpful = false;//top->getWidth() == Expr::Bool;
    bool isSelect = isa<SelectExpr>(top);

    if (!isa<ConstantExpr>(top)) {
      Expr *e = top.get();
      for (unsigned i = 0; i <e->getNumKids(); ++i) {
        ref<Expr> k = e->getKid(i);
        if (isa<ConstantExpr>(k))
          continue;
        if (couldBeHelpful || (isSelect && i == 0)) {
          ConstraintManager::merge_conditions_ty::iterator it =
              constraints.mergeConditions.find(k);
          if (it != constraints.mergeConditions.end()) {
            subqueryConditions.push_back(*it);
            if (subqueryConditions.size() >= SUBQUERY_CONDITIONS_MAX)
              return;
          }
        }
        if (visited.insert(k).second) {
          stack.push_back(k);
        }
      }
    }
  }
}

void *HLParallelSolver::subqueryGeneratorThread(void *arg) {
  HLParallelSolver *pSolver = static_cast<HLParallelSolver*>(arg);

  pthread_mutex_lock(&pSolver->mutex);
  while (true) {
    while(pSolver->queriesInfo.empty() || pSolver->queriesInfo.back()->solved
                || pSolver->queriesInfo.back()->subqueryIndex != ULLONG_MAX)
      pthread_cond_wait(&pSolver->queryReadyCond, &pSolver->mutex);

    QueryInfo *queryInfo = pSolver->queriesInfo.back();
    if (queryInfo->type == ExitRequest) {
      queryInfo->subqueryIndex = 0;
      pthread_cond_broadcast(&pSolver->subqueryReadyCond);
      break;
    }

    assert(!queryInfo->subsolversActive);
    assert(!queryInfo->subsolversDone);
    assert(queryInfo->subqueryIndex == ULLONG_MAX);
    assert(queryInfo->subqueryConditions.empty());
    queryInfo->subsolversActive = 1;

#ifdef __DISABLE_SUBSOLVERS
#warning Subsolvers are disabled!
    if (!queryInfo->isDone()) {
      queryInfo->subsolversActive = 0;
      queryInfo->subsolversDone = true;
      queryInfo->subqueryIndex = ULLONG_MAX-1;
      if (queryInfo->isDone()) {
        pthread_cond_signal(&pSolver->queryDoneCond);
      }
    } else {
      queryInfo->subsolversActive = 0;
    }
    continue;
#endif

    if (!queryInfo->constraints.mergeConditions.empty()) {
      pthread_mutex_unlock(&pSolver->mutex);

    // Calculate subqueryConditions
    // XXX: here we use ad-hoc synchronization for
    // accessing subqueryConditions: it can be written only when
    // subqueryIndex == ULLONG_MAX but it can be read only when otherwise

      queryInfo->gatherSubqueryConditionsFor(queryInfo->query.expr);

      for (ConstraintManager::const_iterator it = queryInfo->constraints.begin(),
                         ie = queryInfo->constraints.end(); it != ie; ++it) {
        if (queryInfo->isDone() ||
                 queryInfo->subqueryConditions.size() >= SUBQUERY_CONDITIONS_MAX)
          break;
        queryInfo->gatherSubqueryConditionsFor(*it);
      }

      assert(queryInfo->subqueryConditions.size() <= SUBQUERY_CONDITIONS_MAX);

      pthread_mutex_lock(&pSolver->mutex);
    }

    queryInfo->subqueryIndex = 0;

    if (!queryInfo->isDone()) {
      queryInfo->subsolversActive = 0;
      std::cerr << "Generated " << queryInfo->subqueryConditions.size()
                << " useful subquery conditions" << std::endl;
      if (!queryInfo->subqueryConditions.empty()) {
        if (queryInfo->type == ValueQuery) {
          findSymbolicObjects(queryInfo->query.expr,
                  const_cast<std::vector<const Array*>& >(queryInfo->objects));
        }
        pthread_cond_broadcast(&pSolver->subqueryReadyCond);
      } else {
        queryInfo->subsolversDone = true;
        queryInfo->subqueryIndex = ULLONG_MAX-1;
        if (queryInfo->isDone()) {
          pSolver->solver->impl->cancelPendingJobs();
          pthread_cond_signal(&pSolver->queryDoneCond);
        }
      }
    } else {
      queryInfo->subsolversActive = 0;
    }
  }
  pthread_mutex_unlock(&pSolver->mutex);
  return NULL;
}

void *HLParallelSolver::subsolverThread(void *arg) {
  HLParallelSolver *pSolver = static_cast<HLParallelSolver*>(arg);

  bool solved;
  ref<Expr> value;
  bool isValid;
  bool isUnsat;
  bool hasSolution;
  std::vector< std::vector<unsigned char> > initialValues;

  bool notDone;
  bool madeSatSubquery;
  bool madeInvalidSubquery;
  bool seenSatSubquery;
  bool seenInvalidSubquery;

  pthread_mutex_lock(&pSolver->mutex);
  while (true) {
    while(pSolver->queriesInfo.empty() || pSolver->queriesInfo.back()->solved ||
          pSolver->queriesInfo.back()->subqueryIndex >=
            (1ul << pSolver->queriesInfo.back()->subqueryConditions.size()))
      pthread_cond_wait(&pSolver->subqueryReadyCond, &pSolver->mutex);

    QueryInfo *queryInfo = pSolver->queriesInfo.back();
    if (queryInfo->type == ExitRequest)
      break;

    assert(!queryInfo->subsolversDone);
    assert(!queryInfo->subqueryConditions.empty());
    queryInfo->subsolversActive += 1;
    uint64_t subqueryIndex = queryInfo->subqueryIndex++;

    madeSatSubquery = madeInvalidSubquery = false;
    seenSatSubquery = queryInfo->seenSatSubquery;
    seenInvalidSubquery = queryInfo->seenInvalidSubquery;
    pthread_mutex_unlock(&pSolver->mutex);

    /*
    SubqueryBuilderVisitor visitor(queryInfo->subqueryConditions, subqueryIndex);
    */
    ConstraintManager cm(queryInfo->constraints);
    bool infeasible = false;
    for (unsigned i = 0; i < queryInfo->subqueryConditions.size(); ++i) {
      ref<Expr> e = queryInfo->subqueryConditions[i];
      if (!(subqueryIndex & (1ul << i)))
        e = Expr::createIsZero(e);
      if (!cm.checkAddConstraint(e)) {
        infeasible = true;
        break;
      }
    }

    //ref<Expr> expr = visitor.visit(queryInfo->query.expr);
    ref<Expr> expr = queryInfo->query.expr;
    if (!infeasible)
        expr = cm.simplifyExpr(expr);

    do {
      solved = notDone = false;
      switch (queryInfo->type) {
      case ValidityQuery:
        assert(!seenSatSubquery || !seenInvalidSubquery);
        assert(queryInfo->query.expr->getWidth() == Expr::Bool);
        solved = isUnsat = isValid = true;
        if (infeasible) {
          madeSatSubquery = madeInvalidSubquery = true;
          break;
        }

        if (!madeSatSubquery) {
          madeSatSubquery = true;
          if (!seenSatSubquery) {
            // Look for false value
            if (ConstantExpr *ce = dyn_cast<ConstantExpr>(expr)) {
              if (ce->isFalse()) {
                solved = true;
                isUnsat = true;
                break;
              }
            }
            //ref<Expr> expr1 =
            //    OrExpr::create(Expr::createIsZero(expr), Expr::createIsZero(cond));
            solved = pSolver->solver->impl->computeTruth(
                Query(cm, Expr::createIsZero(expr)).asOneExpr(), isUnsat);
            break;
          }
        }

        madeInvalidSubquery = true;
        if (seenInvalidSubquery)
          break;

        /* fallthrough */

      case TruthQuery:
        assert(queryInfo->query.expr->getWidth() == Expr::Bool);
        if (infeasible) {
          solved = isValid = true;
          break;
        }
        if (ConstantExpr *ce = dyn_cast<ConstantExpr>(expr)) {
          if (ce->isTrue()) {
            solved = true;
            isValid = true;
            break;
          }
        }
        //expr = OrExpr::create(expr, Expr::createIsZero(cond));
        solved = pSolver->solver->impl->computeTruth(
            Query(cm, expr).asOneExpr(), isValid);
        break;

      case InitialValuesQuery:
        assert(queryInfo->query.expr->getWidth() == Expr::Bool);
        if (infeasible) {
          solved = isValid = true;
          break;
        }
        if (ConstantExpr *ce = dyn_cast<ConstantExpr>(expr)) {
          if (ce->isTrue()) {
            solved = true;
            isValid = true;
            break;
          }
        }
        //expr = OrExpr::create(expr, Expr::createIsZero(cond));
        solved = pSolver->solver->impl->computeInitialValues(
            Query(cm, expr).asOneExpr(), queryInfo->objects,
            initialValues, hasSolution);
        isValid = !hasSolution;
        break;

      case ValueQuery:
        value = ref<Expr>(0);
        if (infeasible) {
          solved = true;
          break;
        }
        if (isa<ConstantExpr>(expr)) {
          solved = pSolver->solver->impl->computeTruth(
              Query(cm, ConstantExpr::create(1,1)).asOneExpr(), isValid);
          if (solved && !isValid)
            value = expr;
        } else {
          solved = pSolver->solver->impl->computeInitialValues(
              Query(cm, ConstantExpr::create(1,1)).asOneExpr(),
              queryInfo->objects,
              initialValues, hasSolution);
          if (solved && hasSolution) {
            Assignment a(queryInfo->objects, initialValues);
            value = a.evaluate(queryInfo->query.expr);
          }
        }
        break;

      case ExitRequest:
        assert(false);
        break;
      }

      pthread_mutex_lock(&pSolver->mutex);

      if (!queryInfo->isDone()) {
        queryInfo->subsolversActive -= 1;

        // The following is actually buggy: we can't set subsolversDone
        // here as we are not sure whether all subsulvers actually terminated
        // or not. We compensate by also verifying subsolversActive in isDone().
        if (subqueryIndex+1 ==
                (1ul << queryInfo->subqueryConditions.size())) {
          if (queryInfo->type != ValidityQuery ||
                (madeSatSubquery && madeInvalidSubquery))
            queryInfo->subsolversDone = true;
        }

        if (!solved)
          queryInfo->subqueriesAreComplete = false;

        // Assuming C1||C2||... = true, we have the following:
        // Q = (Q && C1) || (Q || C2) || ...
        // Also,
        // !Q = (!Q && C1) || (!Q && C2) || ... =
        //    !((Q || !C1) && (Q || !C2) && ...)
        switch (queryInfo->type) {
          case ValidityQuery:
            if (solved) {
              queryInfo->seenSatSubquery |= !isUnsat;
              queryInfo->seenInvalidSubquery |= !isValid;
              seenSatSubquery = queryInfo->seenSatSubquery;
              seenInvalidSubquery = queryInfo->seenInvalidSubquery;
              if (queryInfo->seenSatSubquery && queryInfo->seenInvalidSubquery) {
                // Q is neither VALID nor UNSAT
                std::cerr << "\n\n***Subsolver got solution faster!\n\n" << std::endl;
                queryInfo->solved = true;
                queryInfo->validity = Solver::Unknown;
              } else if (queryInfo->subsolversDone &&
                         queryInfo->subqueriesAreComplete &&
                         !queryInfo->subsolversActive &&
                         madeInvalidSubquery) {
                std::cerr << "\n\n***Subsolver got INVALID solution faster!\n\n" << std::endl;
                queryInfo->solved = true;
                if (!queryInfo->seenInvalidSubquery) {
                  assert(queryInfo->seenSatSubquery);
                  queryInfo->validity = Solver::True;
                } else {
                  assert(!queryInfo->seenSatSubquery);
                  queryInfo->validity = Solver::False;
                }
              } else if (queryInfo->subsolversDone &&
                         !queryInfo->subqueriesAreComplete &&
                         madeInvalidSubquery) {
                std::cerr << "\n\nSubsolver finished without giving a solution\n\n" << std::endl;
              }
            }
            break;
          case TruthQuery:
          case InitialValuesQuery:
            if (solved) {
              if (!isValid) {
                // hasSolution means we got a solution for which Q||!C is false.
                // For this solution,
                // !Q = (!Q && C1) || (!Q && C2) || ... =
                //    !((Q || !C1) && (Q || !C2) && ...) = true
                // Consequently, !Q is SAT hence Q is INVALID
                std::cerr << "\n\n***Subsolver got solution faster!\n\n" << std::endl;
                queryInfo->solved = true;
                queryInfo->isValid = false;
                if (queryInfo->type == InitialValuesQuery)
                  queryInfo->initialValues.swap(initialValues);
              } else if (queryInfo->subsolversDone &&
                         queryInfo->subqueriesAreComplete &&
                         !queryInfo->subsolversActive) {
                // For all C we got Q || !C is provably true. Consequently,
                // !Q = (!Q && C1) || (!Q && C2) || ... =
                //    !((Q || !C1) && (Q || !C2) && ...) = !true
                // consequently, !Q is UNSAT hence Q is VALID
                std::cerr << "\n\n***Subsolver got INVALID solution faster!\n\n" << std::endl;
                queryInfo->solved = true;
                queryInfo->isValid = true;
              } else if (queryInfo->subsolversDone &&
                         !queryInfo->subsolversActive) {
                std::cerr << "\n\n***Subsolver terminated without giving solution\n\n" << std::endl;
              }
            }
            break;
          case ValueQuery:
            if (solved) {
              if(!value.isNull()) {
                //std::cerr << "\n\n***Subsolver got solution faster!\n\n" << std::endl;
                queryInfo->solved = true;
                queryInfo->value = value;
              } else if (queryInfo->subsolversDone &&
                         !queryInfo->subsolversActive) {
                // We should get a solution by enumerating everything!
                assert(!queryInfo->subqueriesAreComplete);
              }
            }
            break;
          default:
            assert(0);
        }

        if (queryInfo->isDone()) {
          pSolver->solver->impl->cancelPendingJobs();
          pthread_cond_signal(&pSolver->queryDoneCond);
        } else if (queryInfo->type == ValidityQuery && !madeInvalidSubquery) {
          /* Validity query requires two subqueries for each index */
          assert(madeSatSubquery);
          queryInfo->subsolversActive += 1;
          notDone = true;
          pthread_mutex_unlock(&pSolver->mutex);
        }
      } else {
        queryInfo->subsolversActive -= 1;
      }
    } while(notDone);

    if (queryInfo->type == ExitRequest)
      break;
  }
  pthread_mutex_unlock(&pSolver->mutex);
  return NULL;
}

bool HLParallelSolver::computeValidity(const Query &query, Solver::Validity &validity) {
  QueryInfo *queryInfo = new QueryInfo(ValidityQuery, query);

  pthread_mutex_lock(&mutex);
  queriesInfo.push_back(queryInfo);
  pthread_cond_broadcast(&queryReadyCond);

  pthread_cond_wait(&queryDoneCond, &mutex);
  assert(queryInfo->isDone());

  validity = queryInfo->validity;
  bool result = queryInfo->solved;

  while (!queriesInfo.empty() && !queriesInfo.front()->isActive()) {
    delete queriesInfo.front();
    queriesInfo.pop_front();
  }

  pthread_mutex_unlock(&mutex);

  return result;
}

bool HLParallelSolver::computeInitialValues(const Query &query,
                          const std::vector<const Array*> &objects,
                          std::vector< std::vector<unsigned char> > &values,
                          bool &hasSolution) {
  QueryInfo *queryInfo = new QueryInfo(InitialValuesQuery, query, objects);

  pthread_mutex_lock(&mutex);
  queriesInfo.push_back(queryInfo);
  pthread_cond_broadcast(&queryReadyCond);

  pthread_cond_wait(&queryDoneCond, &mutex);
  assert(queryInfo->isDone());

  hasSolution = !queryInfo->isValid;
  if (hasSolution)
    values.swap(queryInfo->initialValues);
  bool result = queryInfo->solved;

  while (!queriesInfo.empty() && !queriesInfo.front()->isActive()) {
    delete queriesInfo.front();
    queriesInfo.pop_front();
  }

  pthread_mutex_unlock(&mutex);

  return result;
}

bool HLParallelSolver::computeTruth(const Query &query, bool &isValid) {
  QueryInfo *queryInfo = new QueryInfo(TruthQuery, query);

  pthread_mutex_lock(&mutex);
  queriesInfo.push_back(queryInfo);
  pthread_cond_broadcast(&queryReadyCond);

  pthread_cond_wait(&queryDoneCond, &mutex);
  assert(queryInfo->isDone());

  isValid = queryInfo->isValid;
  bool result = queryInfo->solved;

  while (!queriesInfo.empty() && !queriesInfo.front()->isActive()) {
    delete queriesInfo.front();
    queriesInfo.pop_front();
  }

  pthread_mutex_unlock(&mutex);

  return result;
}

bool HLParallelSolver::computeValue(const Query &query, ref<Expr> &value) {
  QueryInfo *queryInfo = new QueryInfo(ValueQuery, query);

  pthread_mutex_lock(&mutex);
  queriesInfo.push_back(queryInfo);
  pthread_cond_broadcast(&queryReadyCond);

  pthread_cond_wait(&queryDoneCond, &mutex);
  assert(queryInfo->isDone());

  bool result = queryInfo->solved;
  if (result) {
    assert(!queryInfo->value.isNull());
    value = queryInfo->value;
  }

  while (!queriesInfo.empty() && !queriesInfo.front()->isActive()) {
    delete queriesInfo.front();
    queriesInfo.pop_front();
  }

  pthread_mutex_unlock(&mutex);

  return result;
}
#endif

Solver *createHLParallelSolver(Solver* _solver, unsigned subsolversCount) {
  //return new Solver(new HLParallelSolver(_solver, subsolversCount));
  return NULL;
}

} // namespace klee
