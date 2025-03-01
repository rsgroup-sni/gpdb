//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CDistributionSpecHashed.cpp
//
//	@doc:
//		Specification of hashed distribution
//---------------------------------------------------------------------------

#include "gpopt/base/CDistributionSpecHashed.h"

#include "gpopt/base/CCastUtils.h"
#include "gpopt/base/CColRefSet.h"
#include "gpopt/base/CColRefSetIter.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CExpressionPreprocessor.h"
#include "gpopt/operators/CPhysicalMotionBroadcast.h"
#include "gpopt/operators/CPhysicalMotionHashDistribute.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/operators/CScalarIdent.h"
#include "naucrates/dxl/xml/dxltokens.h"
#include "naucrates/traceflags/traceflags.h"


#define GPOPT_DISTR_SPEC_HASHED_EXPRESSIONS (ULONG(5))

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::CDistributionSpecHashed
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CDistributionSpecHashed::CDistributionSpecHashed(CExpressionArray *pdrgpexpr,
												 BOOL fNullsColocated,
												 IMdIdArray *opfamilies)
	: m_pdrgpexpr(pdrgpexpr),
	  m_opfamilies(opfamilies),
	  m_fNullsColocated(fNullsColocated),
	  m_pdshashedEquiv(nullptr),
	  m_equiv_hash_exprs(nullptr)
{
	GPOS_ASSERT(nullptr != pdrgpexpr);
	GPOS_ASSERT(0 < pdrgpexpr->Size());
	if (GPOS_FTRACE(EopttraceConsiderOpfamiliesForDistribution) &&
		nullptr == opfamilies)
	{
		PopulateDefaultOpfamilies();
	}
	GPOS_ASSERT(m_opfamilies == nullptr ||
				m_opfamilies->Size() == m_pdrgpexpr->Size());
}

//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::CDistributionSpecHashed
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CDistributionSpecHashed::CDistributionSpecHashed(
	CExpressionArray *pdrgpexpr, BOOL fNullsColocated,
	CDistributionSpecHashed *pdshashedEquiv, IMdIdArray *opfamilies)
	: m_pdrgpexpr(pdrgpexpr),
	  m_opfamilies(opfamilies),
	  m_fNullsColocated(fNullsColocated),
	  m_pdshashedEquiv(pdshashedEquiv),
	  m_equiv_hash_exprs(nullptr)
{
	GPOS_ASSERT(nullptr != pdrgpexpr);
	if (GPOS_FTRACE(EopttraceConsiderOpfamiliesForDistribution) &&
		nullptr == opfamilies)
	{
		PopulateDefaultOpfamilies();
	}
	GPOS_ASSERT(m_opfamilies == nullptr ||
				m_opfamilies->Size() == m_pdrgpexpr->Size());
}

//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::~CDistributionSpecHashed
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CDistributionSpecHashed::~CDistributionSpecHashed()
{
	m_pdrgpexpr->Release();
	CRefCount::SafeRelease(m_pdshashedEquiv);
	CRefCount::SafeRelease(m_equiv_hash_exprs);
	CRefCount::SafeRelease(m_opfamilies);
}

void
CDistributionSpecHashed::PopulateDefaultOpfamilies()
{
	CMemoryPool *mp = COptCtxt::PoctxtFromTLS()->Pmp();
	CMDAccessor *mda = COptCtxt::PoctxtFromTLS()->Pmda();
	m_opfamilies = GPOS_NEW(mp) IMdIdArray(mp);
	for (ULONG ul = 0; ul < m_pdrgpexpr->Size(); ++ul)
	{
		CExpression *expr = (*m_pdrgpexpr)[ul];
		IMDId *mdid_type = CScalar::PopConvert(expr->Pop())->MdidType();
		IMDId *mdid_opfamily =
			mda->RetrieveType(mdid_type)->GetDistrOpfamilyMdid();
		GPOS_ASSERT(nullptr != mdid_opfamily && mdid_opfamily->IsValid());
		mdid_opfamily->AddRef();
		m_opfamilies->Append(mdid_opfamily);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::PdsCopyWithRemappedColumns
//
//	@doc:
//		Return a copy of the distribution spec with remapped columns
//
//---------------------------------------------------------------------------
CDistributionSpec *
CDistributionSpecHashed::PdsCopyWithRemappedColumns(
	CMemoryPool *mp, UlongToColRefMap *colref_mapping, BOOL must_exist)
{
	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	const ULONG length = m_pdrgpexpr->Size();
	for (ULONG ul = 0; ul < length; ul++)
	{
		CExpression *pexpr = (*m_pdrgpexpr)[ul];
		pdrgpexpr->Append(pexpr->PexprCopyWithRemappedColumns(
			mp, colref_mapping, must_exist));
	}

	if (nullptr == m_pdshashedEquiv)
	{
		return GPOS_NEW(mp)
			CDistributionSpecHashed(pdrgpexpr, m_fNullsColocated);
	}

	// copy equivalent distribution
	CDistributionSpec *pds = m_pdshashedEquiv->PdsCopyWithRemappedColumns(
		mp, colref_mapping, must_exist);
	CDistributionSpecHashed *pdshashed =
		CDistributionSpecHashed::PdsConvert(pds);
	if (nullptr != m_opfamilies)
	{
		m_opfamilies->AddRef();
	}
	// Remapping columns should not change opfamily, used for passing distribution requests in CTEs
	return GPOS_NEW(mp) CDistributionSpecHashed(pdrgpexpr, m_fNullsColocated,
												pdshashed, m_opfamilies);
}


CDistributionSpec *
CDistributionSpecHashed::StripEquivColumns(CMemoryPool *mp)
{
	m_pdrgpexpr->AddRef();
	if (nullptr != m_opfamilies)
	{
		m_opfamilies->AddRef();
	}
	return GPOS_NEW(mp)
		CDistributionSpecHashed(m_pdrgpexpr, m_fNullsColocated, m_opfamilies);
}


BOOL
CDistributionSpecHashed::FDistributionSpecHashedOnlyOnGpSegmentId() const
{
	const ULONG length = m_pdrgpexpr->Size();
	COperator *pop = (*(m_pdrgpexpr))[0]->Pop();

	return length == 1 && pop->Eopid() == COperator::EopScalarIdent &&
		   CScalarIdent::PopConvert(pop)->Pcr()->IsSystemCol() &&
		   CScalarIdent::PopConvert(pop)->Pcr()->Name().Equals(
			   CDXLTokens::GetDXLTokenStr(EdxltokenGpSegmentIdColName));
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::FSatisfies
//
//	@doc:
//		Check if this distribution spec satisfies the given one
//
//---------------------------------------------------------------------------
BOOL
CDistributionSpecHashed::FSatisfies(const CDistributionSpec *pds) const
{
	if (nullptr != m_pdshashedEquiv && m_pdshashedEquiv->FSatisfies(pds))
	{
		return true;
	}

	if (Matches(pds))
	{
		// exact match implies satisfaction
		return true;
	}

	if (EdtAny == pds->Edt() || EdtNonSingleton == pds->Edt())
	{
		// hashed distribution satisfies the "any" and "non-singleton" distribution requirement
		return true;
	}

	if (EdtHashed != pds->Edt())
	{
		return false;
	}

	GPOS_ASSERT(EdtHashed == pds->Edt());

	const CDistributionSpecHashed *pdsHashed =
		dynamic_cast<const CDistributionSpecHashed *>(pds);

	// Assumes that 'this' distribution spec is based on the underlying table
	// structure, whereas 'pds' distribution spec is based on the query
	// structure. Given that table based distribution spec is derived from
	// distribution columns, 'this' distribution spec should never satisfy
	// FDistributionSpecHashedOnlyOnGpSegmentId().
	if (pdsHashed->FDistributionSpecHashedOnlyOnGpSegmentId())
	{
		// If there exist HashSpecEquivExprs(), then we deny the match in order
		// to prevent incorrectly removing a REDISTRIBUTE MOTION. For example,
		// in the following query:
		//
		// SELECT * FROM t t1, t t2 WHERE t1.gp_segment_id=t2.id;
		if (nullptr == pdsHashed->HashSpecEquivExprs())
		{
			return true;
		}
	}

	return FMatchSubset(pdsHashed);
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::FMatchSubset
//
//	@doc:
//		Check if the expressions of the object match a subset of the passed spec's
//		expressions
//
//---------------------------------------------------------------------------
BOOL
CDistributionSpecHashed::FMatchSubset(
	const CDistributionSpecHashed *pdsHashed) const
{
	const ULONG ulOwnExprs = m_pdrgpexpr->Size();
	const ULONG ulOtherExprs = pdsHashed->m_pdrgpexpr->Size();

	if (ulOtherExprs < ulOwnExprs || !FNullsColocatedCompatible(pdsHashed) ||
		!FDuplicateSensitiveCompatible(pdsHashed))
	{
		return false;
	}

	for (ULONG ulOuter = 0; ulOuter < ulOwnExprs; ulOuter++)
	{
		CExpression *pexprOwn = CCastUtils::PexprWithoutBinaryCoercibleCasts(
			(*m_pdrgpexpr)[ulOuter]);
		IMDId *opfamily_own = nullptr;

		BOOL fFound = false;
		CExpressionArrays *equiv_hash_exprs = pdsHashed->HashSpecEquivExprs();
		for (ULONG ulInner = 0; ulInner < ulOtherExprs; ulInner++)
		{
			CExpression *pexprOther =
				CCastUtils::PexprWithoutBinaryCoercibleCasts(
					(*(pdsHashed->m_pdrgpexpr))[ulInner]);
			IMDId *opfamily_other = nullptr;

			if (GPOS_FTRACE(EopttraceConsiderOpfamiliesForDistribution))
			{
				opfamily_own = (*m_opfamilies)[ulOuter];
				opfamily_other = (*pdsHashed->m_opfamilies)[ulInner];
			}

			if (CUtils::Equals(pexprOwn, pexprOther) &&
				CUtils::Equals(opfamily_own, opfamily_other))
			{
				fFound = true;
				break;
			}

			if (GPOS_FTRACE(EopttraceConsiderOpfamiliesForDistribution))
			{
				CMDAccessor *mda = COptCtxt::PoctxtFromTLS()->Pmda();
				IMDId *expr_type_mdid =
					CScalar::PopConvert(pexprOwn->Pop())->MdidType();
				const IMDType *expr_type = mda->RetrieveType(expr_type_mdid);

				if (expr_type->GetDistrOpfamilyMdid() != opfamily_own ||
					expr_type->GetDistrOpfamilyMdid() != opfamily_other)
				{
					// check equiv_hash_exprs only for default opfamilies
					continue;
				}
			}

			if (nullptr != equiv_hash_exprs && equiv_hash_exprs->Size() > 0)
			{
				GPOS_ASSERT(false == fFound);
				CExpressionArray *equiv_distribution_exprs =
					(*equiv_hash_exprs)[ulInner];
				if (CUtils::Contains(equiv_distribution_exprs, pexprOwn))
				{
					fFound = true;
					break;
				}
			}
		}

		if (!fFound)
		{
			return false;
		}
	}

	return true;
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::PdshashedExcludeColumns
//
//	@doc:
//		Return a copy of the distribution spec after excluding the given
//		columns, return NULL if all distribution expressions are excluded
//
//---------------------------------------------------------------------------
CDistributionSpecHashed *
CDistributionSpecHashed::PdshashedExcludeColumns(CMemoryPool *mp,
												 CColRefSet *pcrs)
{
	GPOS_ASSERT(nullptr != pcrs);

	CExpressionArray *pdrgpexprNew = GPOS_NEW(mp) CExpressionArray(mp);
	const ULONG ulExprs = m_pdrgpexpr->Size();
	for (ULONG ul = 0; ul < ulExprs; ul++)
	{
		CExpression *pexpr = (*m_pdrgpexpr)[ul];
		COperator *pop = pexpr->Pop();
		if (COperator::EopScalarIdent == pop->Eopid())
		{
			// we only care here about column identifiers,
			// any more complicated expressions are copied to output
			const CColRef *colref = CScalarIdent::PopConvert(pop)->Pcr();
			if (pcrs->FMember(colref))
			{
				continue;
			}
		}

		pexpr->AddRef();
		pdrgpexprNew->Append(pexpr);
	}

	if (0 == pdrgpexprNew->Size())
	{
		pdrgpexprNew->Release();
		return nullptr;
	}

	return GPOS_NEW(mp)
		CDistributionSpecHashed(pdrgpexprNew, m_fNullsColocated);
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::AppendEnforcers
//
//	@doc:
//		Add required enforcers to dynamic array
//
//---------------------------------------------------------------------------
void
CDistributionSpecHashed::AppendEnforcers(CMemoryPool *mp,
										 CExpressionHandle &,  // exprhdl
										 CReqdPropPlan *
#ifdef GPOS_DEBUG
											 prpp
#endif	// GPOS_DEBUG
										 ,
										 CExpressionArray *pdrgpexpr,
										 CExpression *pexpr)
{
	GPOS_ASSERT(nullptr != mp);
	GPOS_ASSERT(nullptr != prpp);
	GPOS_ASSERT(nullptr != pdrgpexpr);
	GPOS_ASSERT(nullptr != pexpr);
	GPOS_ASSERT(!GPOS_FTRACE(EopttraceDisableMotions));
	GPOS_ASSERT(
		this == prpp->Ped()->PdsRequired() &&
		"required plan properties don't match enforced distribution spec");

	if (GPOS_FTRACE(EopttraceDisableMotionHashDistribute))
	{
		// hash-distribute Motion is disabled
		return;
	}

	// add a hashed distribution enforcer
	// if the distribution spec request isn't met, always enforce nulls colocation

	CDistributionSpecHashed *this_copy = this;
	if (this->FNullsColocated())
	{
		this_copy->AddRef();
	}
	else
	{
		this_copy = this->Copy(mp, true);
	}

	pexpr->AddRef();
	CExpression *pexprMotion = GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CPhysicalMotionHashDistribute(mp, this_copy), pexpr);
	pdrgpexpr->Append(pexprMotion);
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::HashValue
//
//	@doc:
//		Hash function
//
//---------------------------------------------------------------------------
ULONG
CDistributionSpecHashed::HashValue() const
{
	CDistributionSpecHashed *equiv_spec = this->PdshashedEquiv();
	if (nullptr != equiv_spec)
	{
		return equiv_spec->HashValue();
	}

	ULONG ulHash = (ULONG) Edt();

	ULONG ulHashedExpressions =
		std::min(m_pdrgpexpr->Size(), GPOPT_DISTR_SPEC_HASHED_EXPRESSIONS);

	for (ULONG ul = 0; ul < ulHashedExpressions; ul++)
	{
		CExpression *pexpr = (*m_pdrgpexpr)[ul];
		ulHash = gpos::CombineHashes(ulHash, CExpression::HashValue(pexpr));
	}

	if (nullptr != m_opfamilies && m_opfamilies->Size() > 0)
	{
		for (ULONG ul = 0; ul < m_opfamilies->Size(); ul++)
		{
			IMDId *mdid = (*m_opfamilies)[ul];
			ulHash = gpos::CombineHashes(ulHash, mdid->HashValue());
		}
	}

	if (nullptr != m_equiv_hash_exprs && m_equiv_hash_exprs->Size() > 0)
	{
		for (ULONG ul = 0; ul < m_equiv_hash_exprs->Size(); ul++)
		{
			CExpressionArray *equiv_distribution_exprs =
				(*m_equiv_hash_exprs)[ul];
			for (ULONG id = 0; id < equiv_distribution_exprs->Size(); id++)
			{
				CExpression *pexpr = (*equiv_distribution_exprs)[id];
				ulHash =
					gpos::CombineHashes(ulHash, CExpression::HashValue(pexpr));
			}
		}
	}
	return ulHash;
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::PcrsUsed
//
//	@doc:
//		Extract columns used by the distribution spec
//
//---------------------------------------------------------------------------
CColRefSet *
CDistributionSpecHashed::PcrsUsed(CMemoryPool *mp) const
{
	return CUtils::PcrsExtractColumns(mp, m_pdrgpexpr);
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::FMatchHashedDistribution
//
//	@doc:
//		Exact match against given hashed distribution
//
//---------------------------------------------------------------------------
BOOL
CDistributionSpecHashed::FMatchHashedDistribution(
	const CDistributionSpecHashed *pdshashed) const
{
	GPOS_ASSERT(nullptr != pdshashed);

	if (m_pdrgpexpr->Size() != pdshashed->m_pdrgpexpr->Size() ||
		!FNullsColocatedCompatible(pdshashed) ||
		IsDuplicateSensitive() != pdshashed->IsDuplicateSensitive())
	{
		return false;
	}

	if (!CUtils::Equals(m_opfamilies, pdshashed->m_opfamilies))
	{
		return false;
	}

	const ULONG length = m_pdrgpexpr->Size();
	for (ULONG ul = 0; ul < length; ul++)
	{
		CExpressionArrays *all_equiv_exprs = pdshashed->HashSpecEquivExprs();
		CExpressionArray *equiv_distribution_exprs = nullptr;
		if (nullptr != all_equiv_exprs && all_equiv_exprs->Size() > 0)
		{
			equiv_distribution_exprs = (*all_equiv_exprs)[ul];
		}
		CExpression *pexprLeft = (*(pdshashed->m_pdrgpexpr))[ul];
		CExpression *pexprRight = (*m_pdrgpexpr)[ul];
		BOOL fSuccess = CUtils::Equals(pexprLeft, pexprRight);
		if (!fSuccess)
		{
			// if failed to find a equal match in the source distribution expr
			// array, check the equivalent exprs to find a match
			fSuccess = CUtils::Contains(equiv_distribution_exprs, pexprRight);
		}
		if (!fSuccess)
		{
			return fSuccess;
		}
	}

	return true;
}


//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::Matches
//
//	@doc:
//		Match function
//
//---------------------------------------------------------------------------
BOOL
CDistributionSpecHashed::Matches(const CDistributionSpec *pds) const
{
	GPOS_CHECK_STACK_SIZE;

	if (Edt() != pds->Edt())
	{
		return false;
	}

	const CDistributionSpecHashed *pdshashed =
		CDistributionSpecHashed::PdsConvert(pds);

	if (nullptr != m_pdshashedEquiv && m_pdshashedEquiv->Matches(pdshashed))
	{
		return true;
	}

	if (nullptr != pdshashed->PdshashedEquiv() &&
		pdshashed->PdshashedEquiv()->Matches(this))
	{
		return true;
	}

	return FMatchHashedDistribution(pdshashed);
}

BOOL
CDistributionSpecHashed::Equals(const CDistributionSpec *input_spec) const
{
	GPOS_CHECK_STACK_SIZE;

	if (input_spec->Edt() != Edt())
	{
		return false;
	}

	const CDistributionSpecHashed *other_spec =
		CDistributionSpecHashed::PdsConvert(input_spec);

	CDistributionSpecHashed *spec_equiv = this->PdshashedEquiv();
	CDistributionSpecHashed *other_spec_equiv = other_spec->PdshashedEquiv();
	// if one of the spec has equivalent spec and other doesn't, they are not equal
	if ((spec_equiv != nullptr && other_spec_equiv == nullptr) ||
		(spec_equiv == nullptr && other_spec_equiv != nullptr))
	{
		return false;
	}

	BOOL equals = true;
	// if both the specs has equivalent specs, compare them
	if (nullptr != spec_equiv && nullptr != other_spec_equiv)
	{
		equals = spec_equiv->Equals(other_spec_equiv);
	}
	// if the equivalent spec are not equal, the spec objects are not equal
	if (!equals)
	{
		return false;
	}

	if (!CUtils::Equals(m_opfamilies, other_spec->m_opfamilies))
	{
		return false;
	}

	BOOL matches =
		m_fNullsColocated == other_spec->FNullsColocated() &&
		m_is_duplicate_sensitive == other_spec->IsDuplicateSensitive() &&
		m_fSatisfiedBySingleton == other_spec->FSatisfiedBySingleton() &&
		CUtils::Equals(m_pdrgpexpr, other_spec->m_pdrgpexpr);

	if (!matches)
	{
		return false;
	}

	// compare the equivalent expression arrays
	CExpressionArrays *spec_equiv_exprs = m_equiv_hash_exprs;
	CExpressionArrays *other_spec_equiv_exprs =
		other_spec->HashSpecEquivExprs();

	return CUtils::Equals(spec_equiv_exprs, other_spec_equiv_exprs);
}

//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::PdshashedMaximal
//
//	@doc:
//		Return a hashed distribution on the maximal hashable subset of
//		given columns,
//		if all columns are not hashable, return NULL
//
//---------------------------------------------------------------------------
CDistributionSpecHashed *
CDistributionSpecHashed::PdshashedMaximal(CMemoryPool *mp,
										  CColRefArray *colref_array,
										  BOOL fNullsColocated)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(0 < colref_array->Size());

	CColRefArray *pdrgpcrHashable =
		CUtils::PdrgpcrRedistributableSubset(mp, colref_array);
	CDistributionSpecHashed *pdshashed = nullptr;
	if (0 < pdrgpcrHashable->Size())
	{
		CExpressionArray *pdrgpexpr =
			CUtils::PdrgpexprScalarIdents(mp, pdrgpcrHashable);
		pdshashed =
			GPOS_NEW(mp) CDistributionSpecHashed(pdrgpexpr, fNullsColocated);
	}
	pdrgpcrHashable->Release();

	return pdshashed;
}

// check if the distribution key expression are covered by the input
// expression array
BOOL
CDistributionSpecHashed::IsCoveredBy(
	const CExpressionArray *dist_cols_expr_array) const
{
	BOOL covers = false;
	const CDistributionSpecHashed *pds = this;
	while (pds && !covers)
	{
		covers = CUtils::Contains(dist_cols_expr_array, pds->Pdrgpexpr());
		pds = pds->PdshashedEquiv();
	}
	return covers;
}

// iterate over all the distribution exprs keys and create an array holding
// equivalent exprs which correspond to the same distribution based on equivalence
// for example: select * from t1, t2 where t1.a = t2.c and t1.b = t2.d;
// if the resulting spec consists of column t1.a, t1,b, based on the equivalence,
// it implies that t1.a distribution is equivalent to t2.c and t1.b is equivalent to t2.d
// t1.a --> equivalent exprs: [t1.a, t2.c]
// t1.b --> equivalent exprs:[t1.b, t2.d]
void
CDistributionSpecHashed::ComputeEquivHashExprs(
	CMemoryPool *mp, CExpressionHandle &expression_handle)
{
	CExpressionArray *distribution_exprs = m_pdrgpexpr;
	CExpressionArrays *equiv_distribution_all_exprs = m_equiv_hash_exprs;
	if (nullptr == equiv_distribution_all_exprs)
	{
		// array which holds equivalent scalar expression for each of the distribution key
		equiv_distribution_all_exprs = GPOS_NEW(mp) CExpressionArrays(mp);
		for (ULONG distribution_key_idx = 0;
			 distribution_key_idx < distribution_exprs->Size();
			 distribution_key_idx++)
		{
			// array which holds equivalent expression for the current distribution key
			CExpressionArray *equiv_distribution_exprs =
				GPOS_NEW(mp) CExpressionArray(mp);

			CExpression *distribution_expr =
				(*distribution_exprs)[distribution_key_idx];
			CColRefSet *distribution_expr_cols =
				distribution_expr->DeriveUsedColumns();
			// the input expr is always equivalent to itself, so add it to the equivalent expr array
			distribution_expr->AddRef();
			equiv_distribution_exprs->Append(distribution_expr);

			if (1 != distribution_expr_cols->Size())
			{
				// if there are 0 or more than 1 column in the distribution expr, there will be no
				// equivalent columns for the expr
				equiv_distribution_all_exprs->Append(equiv_distribution_exprs);
				continue;
			}

			// there is only one colref in the set
			const CColRef *distribution_colref =
				distribution_expr_cols->PcrAny();
			GPOS_ASSERT(nullptr != distribution_colref);
			CColRefSet *equiv_cols =
				expression_handle.DerivePropertyConstraint()->PcrsEquivClass(
					distribution_colref);
			// if there are equivalent columns, then we have a chance to create equivalent distribution exprs
			if (nullptr != equiv_cols)
			{
				CColRefSetIter equiv_cols_iter(*equiv_cols);
				while (equiv_cols_iter.Advance())
				{
					CColRef *equiv_col = equiv_cols_iter.Pcr();
					if (equiv_col == distribution_colref)
					{
						// skip the current distribution colref as we need to create the scalar
						// condition with its equivalent colrefs
						continue;
					}
					CExpression *predicate_expr_with_inferred_quals =
						CUtils::PexprScalarEqCmp(mp, distribution_colref,
												 equiv_col);
					// add cast on the expressions if required, both the outer and inner hash exprs
					// should be of the same data type else they may be hashed to different segments
					CExpression *predicate_expr_with_casts =
						CCastUtils::PexprAddCast(
							mp, predicate_expr_with_inferred_quals);
					CExpression *original_predicate_expr =
						predicate_expr_with_inferred_quals;
					if (predicate_expr_with_casts)
					{
						predicate_expr_with_inferred_quals->Release();
						original_predicate_expr = predicate_expr_with_casts;
					}
					CExpression *left_distribution_expr =
						CCastUtils::PexprWithoutBinaryCoercibleCasts(
							(*original_predicate_expr)[0]);
					CExpression *right_distribution_expr =
						CCastUtils::PexprWithoutBinaryCoercibleCasts(
							(*original_predicate_expr)[1]);
					// if the predicate is a = b, and a is the current distribution expr,
					// then the equivalent expr is b
					CExpression *equiv_distribution_expr = nullptr;
					if (CUtils::Equals(left_distribution_expr,
									   distribution_expr))
					{
						equiv_distribution_expr = right_distribution_expr;
					}
					else if (CUtils::Equals(right_distribution_expr,
											distribution_expr))
					{
						equiv_distribution_expr = left_distribution_expr;
					}

					// if equivalent distributione expr is found, add it to the array holding equivalent distribution exprs
					if (equiv_distribution_expr)
					{
						equiv_distribution_expr->AddRef();
						equiv_distribution_exprs->Append(
							equiv_distribution_expr);
					}
					CRefCount::SafeRelease(original_predicate_expr);
				}
			}
			equiv_distribution_all_exprs->Append(equiv_distribution_exprs);
		}
	}
	GPOS_ASSERT(equiv_distribution_all_exprs->Size() == m_pdrgpexpr->Size());
	m_equiv_hash_exprs = equiv_distribution_all_exprs;
}

CDistributionSpecHashed *
CDistributionSpecHashed::Copy(CMemoryPool *mp)
{
	if (nullptr != m_pdrgpexpr)
	{
		m_pdrgpexpr->AddRef();
	}

	if (nullptr != m_opfamilies)
	{
		m_opfamilies->AddRef();
	}

	CDistributionSpecHashed *result = GPOS_NEW(mp)
		CDistributionSpecHashed(m_pdrgpexpr, m_fNullsColocated, m_opfamilies);

	if (nullptr != m_pdshashedEquiv)
	{
		result->m_pdshashedEquiv = m_pdshashedEquiv->Copy(mp);
	}

	return result;
}

CDistributionSpecHashed *
CDistributionSpecHashed::Copy(CMemoryPool *mp, BOOL fNullsColocated)
{
	if (nullptr != m_pdrgpexpr)
	{
		m_pdrgpexpr->AddRef();
	}

	if (nullptr != m_opfamilies)
	{
		m_opfamilies->AddRef();
	}

	CDistributionSpecHashed *result = GPOS_NEW(mp)
		CDistributionSpecHashed(m_pdrgpexpr, fNullsColocated, m_opfamilies);

	if (nullptr != m_pdshashedEquiv)
	{
		result->m_pdshashedEquiv = m_pdshashedEquiv->Copy(mp, fNullsColocated);
	}

	return result;
}
//---------------------------------------------------------------------------
//	@function:
//		CDistributionSpecHashed::OsPrint
//
//	@doc:
//		Print function
//
//---------------------------------------------------------------------------
IOstream &
CDistributionSpecHashed::OsPrint(IOstream &os) const
{
	return OsPrintWithPrefix(os, "");
}

IOstream &
CDistributionSpecHashed::OsPrintWithPrefix(IOstream &os,
										   const char *prefix) const
{
	os << this->SzId() << ": [ ";
	const ULONG length = m_pdrgpexpr->Size();
	for (ULONG ul = 0; ul < length; ul++)
	{
		CExpression *hash_expr = (*m_pdrgpexpr)[ul];

		if (!GPOS_FTRACE(EopttracePrintEquivDistrSpecs) && 1 == length &&
			COperator::EopScalarIdent == hash_expr->Pop()->Eopid())
		{
			// just print the scalar ident, without surrounding decoration
			hash_expr->Pop()->OsPrint(os);
		}
		else
		{
			os << *(hash_expr);
			// the expression added a newline, indent the following with the prefix
			os << prefix;
		}
	}
	if (m_fNullsColocated)
	{
		os << ", nulls colocated";
	}
	else
	{
		os << ", nulls not colocated";
	}

	if (m_is_duplicate_sensitive)
	{
		os << ", duplicate sensitive";
	}

	if (!m_fSatisfiedBySingleton)
	{
		os << ", across-segments";
	}

	os << " ]";

	if (nullptr != m_pdshashedEquiv &&
		GPOS_FTRACE(EopttracePrintEquivDistrSpecs))
	{
		os << ", equiv. dist: ";
		m_pdshashedEquiv->OsPrint(os);
	}

	if (nullptr != m_equiv_hash_exprs && m_equiv_hash_exprs->Size() > 0 &&
		GPOS_FTRACE(EopttracePrintEquivDistrSpecs))
	{
		os << "," << std::endl;
		for (ULONG ul = 0; ul < m_equiv_hash_exprs->Size(); ul++)
		{
			CExpressionArray *equiv_distribution_exprs =
				(*m_equiv_hash_exprs)[ul];
			os << "equiv exprs: " << ul << ":";
			for (ULONG id = 0; id < equiv_distribution_exprs->Size(); id++)
			{
				CExpression *equiv_distribution_expr =
					(*equiv_distribution_exprs)[id];
				os << *equiv_distribution_expr << ",";
			}
		}
	}

	if (nullptr != m_opfamilies && m_opfamilies->Size() > 0)
	{
		os << ", opfamilies: ";
		for (ULONG ul = 0; ul < m_opfamilies->Size(); ul++)
		{
			(*m_opfamilies)[ul]->OsPrint(os);
			os << ",";
		}
	}

	return os;
}

CExpressionArrays *
CDistributionSpecHashed::GetAllDistributionExprs(CMemoryPool *mp)
{
	CExpressionArrays *all_distribution_exprs =
		GPOS_NEW(mp) CExpressionArrays(mp);
	CDistributionSpecHashed *spec = this;
	while (spec)
	{
		CExpressionArray *distribution_exprs = spec->Pdrgpexpr();
		distribution_exprs->AddRef();
		all_distribution_exprs->Append(distribution_exprs);
		spec = spec->PdshashedEquiv();
	}

	return all_distribution_exprs;
}

// create a new spec and which marks the other incoming specs
// as equivalent
CDistributionSpecHashed *
CDistributionSpecHashed::Combine(CMemoryPool *mp,
								 CDistributionSpecHashed *other_spec)
{
	if (nullptr == other_spec)
	{
		return this;
	}

	CDistributionSpecHashed *combined_spec = this->Copy(mp);
	other_spec->AddRef();
	CDistributionSpecHashed *temp_spec = combined_spec;

	while (nullptr != temp_spec->m_pdshashedEquiv)
	{
		temp_spec = temp_spec->m_pdshashedEquiv;
	}

	temp_spec->m_pdshashedEquiv = other_spec;

	return combined_spec;
}

// check if the equivalent spec (if any) has no matching columns with the main spec
BOOL
CDistributionSpecHashed::HasCompleteEquivSpec(CMemoryPool *mp) const
{
	CDistributionSpecHashed *pdshashedEquiv = this->PdshashedEquiv();

	if (nullptr != pdshashedEquiv)
	{
		CColRefSet *pcrshashed = this->PcrsUsed(mp);
		CColRefSet *pcrshashedEquiv = pdshashedEquiv->PcrsUsed(mp);

		// it is considered complete, if the equivalent spec has no matching
		// columns with the main spec
		BOOL result = !pcrshashedEquiv->FIntersects(pcrshashed);
		pcrshashed->Release();
		pcrshashedEquiv->Release();
		return result;
	}

	// no equivalent spec - so, it can't be incomplete
	return false;
}

// use given predicates to complete an incomplete spec, if possible
CDistributionSpecHashed *
CDistributionSpecHashed::TryToCompleteEquivSpec(
	CMemoryPool *mp, CDistributionSpecHashed *pdshashed, CExpression *pexprPred,
	CColRefSet *outerRefs)
{
	CExpressionArray *pdrgpexprPred =
		CPredicateUtils::PdrgpexprConjuncts(mp, pexprPred);
	CExpressionArray *pdrgpexprResult = GPOS_NEW(mp) CExpressionArray(mp);
	CExpressionArray *pdrgpexprHashed = pdshashed->Pdrgpexpr();
	const ULONG size = pdrgpexprHashed->Size();

	ULONG num_orig_exprs_used = 0;
	for (ULONG ul = 0; ul < size; ul++)
	{
		CExpression *pexpr = (*pdrgpexprHashed)[ul];
		CExpression *pexprMatching =
			CUtils::PexprMatchEqualityOrINDF(pexpr, pdrgpexprPred);
		if (nullptr != pexprMatching &&
			outerRefs->FIntersects(pexprMatching->DeriveUsedColumns()))
		{
			// we are able to replace an original expression with one that refers to outer
			// references (values from the equivalent table), making it more complete
			pexprMatching->AddRef();
			pdrgpexprResult->Append(pexprMatching);
		}
		else
		{
			++num_orig_exprs_used;
			pexpr->AddRef();
			pdrgpexprResult->Append(pexpr);
		}
	}
	pdrgpexprPred->Release();

	// don't create an equiv spec, if it uses the same exprs as the original
	if (num_orig_exprs_used == size)
	{
		pdrgpexprResult->Release();
		return nullptr;
	}

	GPOS_ASSERT(pdrgpexprResult->Size() == pdrgpexprHashed->Size());

	// create a matching hashed distribution request
	BOOL fNullsColocated = pdshashed->FNullsColocated();
	CDistributionSpecHashed *pdshashedEquiv =
		GPOS_NEW(mp) CDistributionSpecHashed(pdrgpexprResult, fNullsColocated);

	return pdshashedEquiv;
}

CDistributionSpecHashed *
CDistributionSpecHashed::MakeHashedDistrSpec(
	CMemoryPool *mp, CExpressionArray *pdrgpexpr, BOOL fNullsColocated,
	CDistributionSpecHashed *pdshashedEquiv, IMdIdArray *opfamilies)
{
	if (GPOS_FTRACE(EopttraceConsiderOpfamiliesForDistribution) &&
		nullptr == opfamilies)
	{
		CMDAccessor *mda = COptCtxt::PoctxtFromTLS()->Pmda();
		opfamilies = GPOS_NEW(mp) IMdIdArray(mp);
		for (ULONG ul = 0; ul < pdrgpexpr->Size(); ++ul)
		{
			CExpression *expr = (*pdrgpexpr)[ul];
			IMDId *mdid_type = CScalar::PopConvert(expr->Pop())->MdidType();
			IMDId *mdid_opfamily =
				mda->RetrieveType(mdid_type)->GetDistrOpfamilyMdid();
			if (nullptr == mdid_opfamily)
			{
				opfamilies->Release();
				return nullptr;
			}
			mdid_opfamily->AddRef();
			opfamilies->Append(mdid_opfamily);
		}
	}
	return GPOS_NEW(mp) CDistributionSpecHashed(pdrgpexpr, fNullsColocated,
												pdshashedEquiv, opfamilies);
}
// EOF
