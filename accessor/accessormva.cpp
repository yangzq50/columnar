// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "accessormva.h"
#include "accessortraits.h"
#include "buildermva.h"
#include "reader.h"

#include <algorithm>

namespace columnar
{

template<bool LEFT_CLOSED, bool RIGHT_CLOSED>
class MvaAll_T
{
public:
	template<typename T>
	static FORCE_INLINE bool Test ( const Span_T<T> & dValues, const Span_T<int64_t> & dTestValues )
	{
		if ( dValues.empty() || dTestValues.empty() )
			return false;

		for ( auto i : dValues )
			if ( !std::binary_search ( dTestValues.begin(), dTestValues.end(), i ) )
				return false;

		return true;
	}

	template<typename T>
	static FORCE_INLINE bool Test ( const Span_T<T> & dValues, int64_t iTestValue )
	{
		for ( auto i : dValues )
			if ( i!=iTestValue )
				return false;

		return true;
	}

	template<typename T>
	static FORCE_INLINE bool Test ( const Span_T<T> & dValues, int64_t iMinValue, int64_t iMaxValue )
	{
		if ( dValues.empty() )
			return false;

		int64_t iFirst = dValues.front();
		int64_t iLast = dValues.back() ;
		return ( LEFT_CLOSED ? ( iFirst>=iMinValue ) : ( iFirst>iMinValue ) ) && ( RIGHT_CLOSED ? ( iLast<=iMaxValue ) : ( iLast<iMaxValue ) );
	}
};


template<bool LEFT_CLOSED, bool RIGHT_CLOSED>
class MvaAny_T
{
public:
	template <typename T>
	static inline bool Test ( const Span_T<T> & dValues, const Span_T<int64_t> & dTestValues )
	{
		if ( dValues.empty() || dTestValues.empty() )
			return false;

		const T * pLeft = dValues.data();

		for ( auto iTestValue : dTestValues )
		{
			const T * pRight = &dValues.back();
			while ( pLeft<=pRight )
			{
				const T * pValue = pLeft + (pRight - pLeft) / 2;
				T iValue = *pValue;
				if ( iValue < iTestValue )
					pLeft = pValue + 1;
				else if ( iValue > iTestValue )
					pRight = pValue - 1;
				else
					return true;
			}
		}

		return false;
	}

	template <typename T>
	static inline bool Test ( const Span_T<T> & dValues, int64_t iTestValue )
	{
		return std::binary_search ( dValues.begin(), dValues.end(), (T)iTestValue );
	}

	template<typename T>
	static FORCE_INLINE bool Test ( const Span_T<T> & dValues, int64_t iMinValue, int64_t iMaxValue )
	{
		if ( dValues.empty() )
			return false;

		const T * pEnd = dValues.data()+dValues.size();
		const T * pLeft = dValues.data();
		const T * pRight = pEnd - 1;

		while ( pLeft <= pRight )
		{
			const T * pValue = pLeft + ( pRight - pLeft ) / 2;
			T iValue = *pValue;

			if ( iValue < iMinValue )
				pLeft = pValue + 1;
			else if ( iValue > iMinValue )
				pRight = pValue - 1;
			else
				return LEFT_CLOSED || pValue+1<pEnd;
		}

		if ( pLeft==pEnd )
			return false;

		if ( RIGHT_CLOSED )
			return *pLeft<=iMaxValue;

		return *pLeft<iMaxValue;
	}
};

/////////////////////////////////////////////////////////////////////

template <typename T>
static FORCE_INLINE void ApplyInverseDeltas ( Span_T<T> & dValues, std::vector<Span_T<T>> & dValuePtrs )
{
	for ( auto & tValue : dValuePtrs )
	{
		T * pData = tValue.data();
		int iLen = (int)tValue.size();
		if ( !iLen )
			continue;

		int i = 1;
		for ( ; i<iLen-1; i+=2 )
		{
			pData[i] += pData[i-1];
			pData[i+1] += pData[i];
		}

		for ( ; i!=iLen; i++ )
			pData[i] += pData[i-1];
	}
}

template <typename T, bool PACK>
static FORCE_INLINE uint32_t PackMVA ( const Span_T<T> & dValue, const uint8_t * & pValue )
{
	if ( PACK )
		pValue = ByteCodec_c::PackData(dValue);
	else
		pValue = (const uint8_t*)dValue.data();

	return uint32_t ( dValue.size()*sizeof(T) );
}

template <typename T>
static FORCE_INLINE void PrecalcSizeOffset ( const Span_T<uint32_t> & dLengths, Span_T<T> & dValues, std::vector<Span_T<T>> & dValuePtrs )
{
	// FIXME! optimize
	dValuePtrs.resize ( dLengths.size() );
	uint32_t uOffset = 0;
	for ( size_t i = 0; i<dLengths.size(); i++ )
	{
		uint32_t uSize = dLengths[i];
		if ( uSize )
			dValuePtrs[i] = Span_T<T> ( &dValues[uOffset], uSize );
		else
			dValuePtrs[i] = Span_T<T>();

		uOffset += uSize;
	}
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class StoredBlock_MvaConst_T
{
public:
						StoredBlock_MvaConst_T ( const std::string & sCodec32, const std::string & sCodec64 );

	FORCE_INLINE void	ReadHeader ( FileReader_c & tReader );

	template <bool PACK>
	FORCE_INLINE uint32_t	GetValue ( const uint8_t * & pValue ) const;

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<T>			m_dValue;
	Span_T<T>					m_dValueSpan;
	SpanResizeable_T<uint32_t>	m_dTmp;
};

template <typename T>
StoredBlock_MvaConst_T<T>::StoredBlock_MvaConst_T ( const std::string & sCodec32, const std::string & sCodec64 )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) )
{}

template <typename T>
void StoredBlock_MvaConst_T<T>::ReadHeader ( FileReader_c & tReader )
{
	uint32_t uSize = tReader.Unpack_uint32();
	DecodeValues_PFOR ( m_dValue, tReader, *m_pCodec, m_dTmp, uSize );
	ComputeInverseDeltas ( m_dValue, true );
	m_dValueSpan = m_dValue;
}

template <typename T>
template <bool PACK>
uint32_t StoredBlock_MvaConst_T<T>::GetValue ( const uint8_t * & pValue ) const
{
	return PackMVA<T,PACK> ( m_dValueSpan, pValue );
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class StoredBlock_MvaConstLen_T
{
public:
						StoredBlock_MvaConstLen_T ( const std::string & sCodec32, const std::string & sCodec64 );

	FORCE_INLINE void	ReadHeader ( FileReader_c & tReader );
	FORCE_INLINE void	ReadSubblock ( int iSubblockId, int iSubblockValues, FileReader_c & tReader );

	template <bool PACK>
	FORCE_INLINE uint32_t	GetValue ( const uint8_t * & pValue, int iIdInSubblock ) const;
	FORCE_INLINE const std::vector<Span_T<T>> & GetAllValues() const { return m_dValuePtrs; }

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dSubblockCumulativeSizes;
	SpanResizeable_T<uint32_t>	m_dTmp;

	SpanResizeable_T<T>			m_dValues;
	std::vector<Span_T<T>>		m_dValuePtrs;

	int							m_iLength = 0;
	int64_t						m_tValuesOffset = 0;
	int							m_iSubblockId = -1;

	FORCE_INLINE void			PrecalcSizeOffset( int iNumSubblockValues );
};

template <typename T>
StoredBlock_MvaConstLen_T<T>::StoredBlock_MvaConstLen_T ( const std::string & sCodec32, const std::string & sCodec64 )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) )
{}

template <typename T>
void StoredBlock_MvaConstLen_T<T>::ReadHeader ( FileReader_c & tReader )
{
	m_iLength = tReader.Unpack_uint32();
	uint32_t uSubblockSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dSubblockCumulativeSizes, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	m_tValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}

template <typename T>
void StoredBlock_MvaConstLen_T<T>::ReadSubblock ( int iSubblockId, int iNumSubblockValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	uint32_t uSize = m_dSubblockCumulativeSizes[iSubblockId];
	uint32_t uOffset = 0;
	if ( iSubblockId>0 )
	{
		uOffset = m_dSubblockCumulativeSizes[iSubblockId-1];
		uSize -= uOffset;
	}

	tReader.Seek ( m_tValuesOffset+uOffset );

	int iValuesInSubblock = m_iLength*iNumSubblockValues;
	m_dValues.resize(iValuesInSubblock);
	DecodeValues_PFOR ( m_dValues, tReader, *m_pCodec, m_dTmp, uSize );

	PrecalcSizeOffset(iNumSubblockValues);
	ApplyInverseDeltas ( m_dValues, m_dValuePtrs );
}

template <typename T>
void StoredBlock_MvaConstLen_T<T>::PrecalcSizeOffset( int iNumSubblockValues )
{
	m_dValuePtrs.resize(iNumSubblockValues);
	uint32_t uOffset = 0;
	for ( auto & i : m_dValuePtrs )
	{
		i = Span_T<T> ( &m_dValues[uOffset], m_iLength );
		uOffset += m_iLength;
	}
}

template <typename T>
template <bool PACK>
uint32_t StoredBlock_MvaConstLen_T<T>::GetValue ( const uint8_t * & pValue, int iIdInSubblock ) const
{
	return PackMVA<T,PACK> ( m_dValuePtrs[iIdInSubblock], pValue );
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class StoredBlock_MvaTable_T
{
public:
								StoredBlock_MvaTable_T ( const std::string & sCodec32, const std::string & sCodec64, int iSubblockSize );

	FORCE_INLINE void			ReadHeader ( FileReader_c & tReader, uint32_t uDocsInBlock );
	FORCE_INLINE void			ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader );

	template <bool PACK>
	FORCE_INLINE uint32_t		GetValue ( int iIdInSubblock, FileReader_c & tReader, const uint8_t * & pValue );
	FORCE_INLINE const Span_T<uint32_t> & GetValueIndexes() const { return m_tValuesRead; }

	template <typename T_COMP>
	FORCE_INLINE Span_T<T_COMP>	GetValueFromTable ( uint8_t uIndex ) const { return { (T_COMP*)m_dValuePtrs[uIndex].data(), m_dValuePtrs[uIndex].size() }; }
	FORCE_INLINE int			GetTableSize() const { return (int)m_dValuePtrs.size(); }

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dTmp;

	SpanResizeable_T<uint32_t>	m_dLengths;
	SpanResizeable_T<T>			m_dValues;
	std::vector<Span_T<T>>		m_dValuePtrs;

	int64_t						m_iValuesOffset = 0;
	int							m_iSubblockId = -1;
	int							m_iBits = 0;
	std::vector<uint32_t>		m_dValueIndexes;
	std::vector<uint32_t>		m_dEncoded;

	Span_T<uint32_t>			m_tValuesRead;
};

template <typename T>
StoredBlock_MvaTable_T<T>::StoredBlock_MvaTable_T ( const std::string & sCodec32, const std::string & sCodec64, int iSubblockSize )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) )
{
	m_dValueIndexes.resize(iSubblockSize);
}

template <typename T>
void StoredBlock_MvaTable_T<T>::ReadHeader ( FileReader_c & tReader, uint32_t uDocsInBlock )
{
	uint32_t uSizeOfLengths = tReader.Unpack_uint32();
	DecodeValues_PFOR ( m_dLengths, tReader, *m_pCodec, m_dTmp, uSizeOfLengths );

	uint32_t uSizeOfValues = tReader.Unpack_uint32();
	uint32_t uTotalLength = 0;
	for ( auto i : m_dLengths )
		uTotalLength += i;

	m_dValues.resize(uTotalLength);
	DecodeValues_PFOR ( m_dValues, tReader, *m_pCodec, m_dTmp, uSizeOfValues );

	PrecalcSizeOffset ( m_dLengths, m_dValues, m_dValuePtrs );
	ApplyInverseDeltas ( m_dValues, m_dValuePtrs );

	m_iBits = CalcNumBits ( m_dValuePtrs.size() );
	m_dEncoded.resize ( ( m_dValueIndexes.size() >> 5 ) * m_iBits );

	m_iValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}

template <typename T>
void StoredBlock_MvaTable_T<T>::ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	size_t uPackedSize = m_dEncoded.size()*sizeof ( m_dEncoded[0] );
	tReader.Seek ( m_iValuesOffset + uPackedSize*iSubblockId );
	tReader.Read ( (uint8_t*)m_dEncoded.data(), uPackedSize );
	BitUnpack128 ( m_dEncoded, m_dValueIndexes, m_iBits );

	m_tValuesRead = { m_dValueIndexes.data(), (size_t)iNumValues };
}

template <typename T>
template <bool PACK>
uint32_t StoredBlock_MvaTable_T<T>::GetValue ( int iIdInSubblock, FileReader_c & tReader, const uint8_t * & pValue )
{
	return PackMVA<T,PACK> ( m_dValuePtrs [ m_dValueIndexes[iIdInSubblock] ], pValue );
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class StoredBlock_MvaPFOR_T
{
public:
							StoredBlock_MvaPFOR_T ( const std::string & sCodec32, const std::string & sCodec64 );

	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader );
	FORCE_INLINE void		ReadSubblock ( int iSubblockId, FileReader_c & tReader );

	template <bool PACK>
	FORCE_INLINE uint32_t		GetValue ( const uint8_t * & pValue, int iIdInSubblock ) const;
	FORCE_INLINE const std::vector<Span_T<T>> & GetAllValues() const { return m_dValuePtrs; }

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dSubblockCumulativeSizes;
	SpanResizeable_T<uint32_t>	m_dTmp;

	SpanResizeable_T<uint32_t>	m_dLengths;
	SpanResizeable_T<T>			m_dValues;
	std::vector<Span_T<T>>		m_dValuePtrs;

	int64_t						m_tValuesOffset = 0;
	int							m_iSubblockId = -1;
};

template <typename T>
StoredBlock_MvaPFOR_T<T>::StoredBlock_MvaPFOR_T ( const std::string & sCodec32, const std::string & sCodec64 )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) )
{}

template <typename T>
void StoredBlock_MvaPFOR_T<T>::ReadHeader ( FileReader_c & tReader )
{
	uint32_t uSubblockSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dSubblockCumulativeSizes, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	m_tValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}

template <typename T>
void StoredBlock_MvaPFOR_T<T>::ReadSubblock ( int iSubblockId, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	uint32_t uSize = m_dSubblockCumulativeSizes[iSubblockId];
	uint32_t uOffset = 0;
	if ( iSubblockId>0 )
	{
		uOffset = m_dSubblockCumulativeSizes[iSubblockId-1];
		uSize -= uOffset;
	}

	tReader.Seek ( m_tValuesOffset+uOffset );

	int64_t iOffset = tReader.GetPos();
	uint32_t uSize1 = tReader.Unpack_uint32();
	int64_t iDelta  = tReader.GetPos()-iOffset;

	DecodeValues_PFOR ( m_dLengths, tReader, *m_pCodec, m_dTmp, uSize1 );
	uint32_t uTotalLength = 0;
	for ( auto i : m_dLengths )
		uTotalLength += i;

	m_dValues.resize(uTotalLength);
	DecodeValues_PFOR ( m_dValues, tReader, *m_pCodec, m_dTmp, uint32_t ( uSize-uSize1-iDelta ) );

	PrecalcSizeOffset ( m_dLengths, m_dValues, m_dValuePtrs );
	ApplyInverseDeltas ( m_dValues, m_dValuePtrs );
}

template <typename T>
template <bool PACK>
FORCE_INLINE uint32_t StoredBlock_MvaPFOR_T<T>::GetValue ( const uint8_t * & pValue, int iIdInSubblock ) const
{
	return PackMVA<T,PACK> ( m_dValuePtrs[iIdInSubblock], pValue );
}

//////////////////////////////////////////////////////////////////////////

template<typename T>
class Accessor_MVA_T : public StoredBlockTraits_t
{
	using BASE = StoredBlockTraits_t;

public:
			Accessor_MVA_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader );

protected:
	const AttributeHeader_i &		m_tHeader;
	std::unique_ptr<FileReader_c>	m_pReader;

	StoredBlock_MvaConst_T<T>		m_tBlockConst;
	StoredBlock_MvaConstLen_T<T>	m_tBlockConstLen;
	StoredBlock_MvaTable_T<T>		m_tBlockTable;
	StoredBlock_MvaPFOR_T<T>		m_tBlockPFOR;

	void (Accessor_MVA_T::*m_fnReadValue)() = nullptr;
	void (Accessor_MVA_T::*m_fnReadValuePacked)() = nullptr;

	MvaPacking_e					m_ePacking = MvaPacking_e::CONST;

	const uint8_t *					m_pResult = nullptr;
	size_t							m_tValueLength = 0;

	FORCE_INLINE void				SetCurBlock ( uint32_t uBlockId );

	template <bool PACK> void		ReadValue_Const();
	template <bool PACK> void		ReadValue_ConstLen();
	template <bool PACK> void		ReadValue_Table();
	template <bool PACK> void		ReadValue_PFOR();
};

template<typename T>
Accessor_MVA_T<T>::Accessor_MVA_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
	: StoredBlockTraits_t ( tHeader.GetSettings().m_iSubblockSizeMva )
	, m_tHeader ( tHeader )
	, m_pReader ( pReader )
	, m_tBlockConst ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
	, m_tBlockConstLen ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
	, m_tBlockTable ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64, tHeader.GetSettings().m_iSubblockSizeMva )
	, m_tBlockPFOR ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
{
	assert(pReader);
}

template<typename T>
void Accessor_MVA_T<T>::SetCurBlock ( uint32_t uBlockId )
{
	m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
	m_ePacking = (MvaPacking_e)m_pReader->Unpack_uint32();

	uint32_t uDocsInBlock = m_tHeader.GetNumDocs(uBlockId);

	switch ( m_ePacking )
	{
	case MvaPacking_e::CONST:
		m_fnReadValue		= &Accessor_MVA_T<T>::ReadValue_Const<false>;
		m_fnReadValuePacked = &Accessor_MVA_T<T>::ReadValue_Const<true>;
		m_tBlockConst.ReadHeader ( *m_pReader );
		break;

	case MvaPacking_e::CONSTLEN:
		m_fnReadValue		= &Accessor_MVA_T<T>::ReadValue_ConstLen<false>;
		m_fnReadValuePacked = &Accessor_MVA_T<T>::ReadValue_ConstLen<true>;
		m_tBlockConstLen.ReadHeader ( *m_pReader );
		break;

	case MvaPacking_e::TABLE:
		m_fnReadValue		= &Accessor_MVA_T<T>::ReadValue_Table<false>;
		m_fnReadValuePacked = &Accessor_MVA_T<T>::ReadValue_Table<true>;
		m_tBlockTable.ReadHeader ( *m_pReader, uDocsInBlock );
		break;

	case MvaPacking_e::DELTA_PFOR:
		m_fnReadValue		= &Accessor_MVA_T<T>::ReadValue_PFOR<false>;
		m_fnReadValuePacked = &Accessor_MVA_T<T>::ReadValue_PFOR<true>;
		m_tBlockPFOR.ReadHeader ( *m_pReader );
		break;

	default:
		assert ( 0 && "Packing not implemented yet" );
		break;
	}

	m_tRequestedRowID = INVALID_ROW_ID;
	m_pResult = nullptr;

	SetBlockId ( uBlockId, uDocsInBlock );
}

template <typename T>
template <bool PACK>
void Accessor_MVA_T<T>::ReadValue_Const()
{
	m_tValueLength = m_tBlockConst.template GetValue<PACK>(m_pResult);
}

template <typename T>
template <bool PACK>
void Accessor_MVA_T<T>::ReadValue_ConstLen()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	int iSubblockId = GetSubblockId(uIdInBlock);
	m_tBlockConstLen.ReadSubblock ( iSubblockId, BASE::GetNumSubblockValues(iSubblockId), *m_pReader );
	m_tValueLength = m_tBlockConstLen.template GetValue<PACK> ( m_pResult, GetValueIdInSubblock(uIdInBlock) );
}

template <typename T>
template <bool PACK>
void Accessor_MVA_T<T>::ReadValue_Table()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	int iSubblockId = StoredBlockTraits_t::GetSubblockId(uIdInBlock);
	m_tBlockTable.ReadSubblock ( iSubblockId, StoredBlockTraits_t::GetNumSubblockValues(iSubblockId), *m_pReader );
	m_tValueLength = m_tBlockTable.template GetValue<PACK>( GetValueIdInSubblock(uIdInBlock), *m_pReader, m_pResult );
}

template <typename T>
template <bool PACK>
void Accessor_MVA_T<T>::ReadValue_PFOR()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	m_tBlockPFOR.ReadSubblock ( GetSubblockId(uIdInBlock), *m_pReader );
	m_tValueLength = m_tBlockPFOR.template GetValue<PACK> ( m_pResult, GetValueIdInSubblock(uIdInBlock) );
}

//////////////////////////////////////////////////////////////////////////


template <typename T>
class Iterator_MVA_T : public Iterator_i, public Accessor_MVA_T<T>
{
	using BASE = Accessor_MVA_T<T>;
	using BASE::Accessor_MVA_T;

public:
	uint32_t	AdvanceTo ( uint32_t tRowID ) final;

	int64_t		Get() final;
	int			Get ( const uint8_t * & pData, bool bPack ) final;
	int			GetLength() const final;

	uint64_t	GetStringHash() final { return 0; }
	bool		HaveStringHashes() const final { return false; }
};

template <typename T>
uint32_t Iterator_MVA_T<T>::AdvanceTo ( uint32_t tRowID )
{
	uint32_t uBlockId = RowId2BlockId(tRowID);
	if ( uBlockId!=BASE::m_uBlockId )
		BASE::SetCurBlock(uBlockId);

	BASE::m_tRequestedRowID = tRowID;

	return tRowID;
}

template <typename T>
int64_t Iterator_MVA_T<T>::Get()
{
	assert ( 0 && "INTERNAL ERROR: requesting int from MVA iterator" );
	return 0;
}

template <typename T>
int Iterator_MVA_T<T>::Get ( const uint8_t * & pData, bool bPack )
{
	if ( bPack )
	{
		assert(BASE::m_fnReadValuePacked);
		(*this.*BASE::m_fnReadValuePacked)();
	}
	else
	{
		assert(BASE::m_fnReadValue);
		(*this.*BASE::m_fnReadValue)();
	}

	pData = BASE::m_pResult;
	BASE::m_pResult = nullptr;

	return (int)BASE::m_tValueLength;
}

template <typename T>
int Iterator_MVA_T<T>::GetLength() const
{
	assert ( 0 && "INTERNAL ERROR: requesting blob length from MVA iterator" );
	return 0;
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_MVA_c : public Filter_t
{
public:
				AnalyzerBlock_MVA_c ( uint32_t & tRowID );

	void		Setup ( const Filter_t & tSettings );

protected:
	uint32_t &	m_tRowID;
	int64_t		m_iValue = 0;
};


AnalyzerBlock_MVA_c::AnalyzerBlock_MVA_c ( uint32_t & tRowID )
	: m_tRowID ( tRowID )
{}


void AnalyzerBlock_MVA_c::Setup ( const Filter_t & tSettings )
{
	assert ( tSettings.m_eMvaAggr!=MvaAggr_e::NONE );
	*(Filter_t*)this = tSettings;

	if ( m_dValues.size()==1 )
		m_iValue = m_dValues[0];
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_MVA_Const_c : public AnalyzerBlock_MVA_c
{
	using AnalyzerBlock_MVA_c::AnalyzerBlock_MVA_c;

public:
	template<typename T, typename T_COMP, typename FUNC>
	FORCE_INLINE bool	SetupNextBlock ( const StoredBlock_MvaConst_T<T> & tBlock );
	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, int iNumValues );
};

template<typename T, typename T_COMP, typename FUNC>
bool AnalyzerBlock_MVA_Const_c::SetupNextBlock ( const StoredBlock_MvaConst_T<T> & tBlock )
{
	const uint8_t * pValue = nullptr;
	uint32_t uLength = tBlock.template GetValue<false>(pValue);
	Span_T<T_COMP> tCheck ( (T_COMP*)pValue, uLength/sizeof(T_COMP) );

	switch ( m_eType )
	{
	case FilterType_e::VALUES:
		if ( m_dValues.size()==1 )
			return FUNC::Test ( tCheck, m_iValue );

		return FUNC::Test ( tCheck, m_dValues );

	case FilterType_e::RANGE:
		if ( FUNC::Test ( tCheck, m_iMinValue, m_iMaxValue ) )
			return true;

		break;

	default:
		break;
	}

	return false;
}


int AnalyzerBlock_MVA_Const_c::ProcessSubblock ( uint32_t * & pRowID, int iNumValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( int i = 0; i < iNumValues; i++ )
		*pRowID++ = tRowID++;

	m_tRowID = tRowID;
	return iNumValues;
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_MVA_Table_c : public AnalyzerBlock_MVA_c
{
	using AnalyzerBlock_MVA_c::AnalyzerBlock_MVA_c;

public:
	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes );

	template<typename T, typename T_COMP, typename FUNC>
	FORCE_INLINE bool	SetupNextBlock ( const StoredBlock_MvaTable_T<T> & tBlock );

private:
	std::array<bool, UCHAR_MAX> m_dMap;
};


int AnalyzerBlock_MVA_Table_c::ProcessSubblock ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes )
{
	uint32_t tRowID = m_tRowID;

	for ( auto i : dValueIndexes )
	{
		if ( m_dMap[i] )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValueIndexes.size();
}

template<typename T, typename T_COMP, typename FUNC>
bool AnalyzerBlock_MVA_Table_c::SetupNextBlock ( const StoredBlock_MvaTable_T<T> & tBlock )
{
	bool bAnythingMatches = false;

	switch ( m_eType )
	{
	case FilterType_e::VALUES:
		if ( m_dValues.size()==1 )
		{
			for ( int i = 0; i < tBlock.GetTableSize(); i++ )
			{
				m_dMap[i] = FUNC::Test ( tBlock.template GetValueFromTable<T_COMP>(i), m_iValue );
				bAnythingMatches |= m_dMap[i];
			}
		}
		else
		{
			for ( int i = 0; i < tBlock.GetTableSize(); i++ )
			{
				m_dMap[i] = FUNC::Test ( tBlock.template GetValueFromTable<T_COMP>(i), m_dValues );
				bAnythingMatches |= m_dMap[i];
			}
		}
		break;

	case FilterType_e::RANGE:
		for ( int i = 0; i < tBlock.GetTableSize(); i++ )
		{
			m_dMap[i] = FUNC::Test ( tBlock.template GetValueFromTable<T_COMP>(i), m_iMinValue, m_iMaxValue );
			bAnythingMatches |= m_dMap[i];
		}
		break;

	default:
		break;
	}

	return bAnythingMatches;
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_MVA_Values_c : public AnalyzerBlock_MVA_c
{
	using AnalyzerBlock_MVA_c::AnalyzerBlock_MVA_c;

public:
	template<typename T, typename T_COMP, typename FUNC>
	FORCE_INLINE int	ProcessSubblock_SingleValue ( uint32_t * & pRowID, const std::vector<Span_T<T>> & dValues );

	template<typename T, typename T_COMP, typename FUNC>
	FORCE_INLINE int	ProcessSubblock_Values ( uint32_t * & pRowID, const std::vector<Span_T<T>> & dValues );

	template<typename T, typename T_COMP, typename FUNC>
	FORCE_INLINE int	ProcessSubblock_Range ( uint32_t * & pRowID, const std::vector<Span_T<T>> & dValues );
};


template<typename T, typename T_COMP, typename FUNC>
int AnalyzerBlock_MVA_Values_c::ProcessSubblock_SingleValue ( uint32_t * & pRowID, const std::vector<Span_T<T>> & dValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( const auto & i : dValues )
	{
		if ( FUNC::Test ( Span_T<T_COMP> ( (T_COMP*)i.data(), i.size() ), m_iValue ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

template<typename T, typename T_COMP, typename FUNC>
int AnalyzerBlock_MVA_Values_c::ProcessSubblock_Values ( uint32_t * & pRowID, const std::vector<Span_T<T>> & dValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( const auto & i : dValues )
	{
		if ( FUNC::Test ( Span_T<T_COMP> ( (T_COMP*)i.data(), i.size() ), m_dValues ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

template<typename T, typename T_COMP, typename FUNC>
int AnalyzerBlock_MVA_Values_c::ProcessSubblock_Range ( uint32_t * & pRowID, const std::vector<Span_T<T>> & dValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( const auto & i : dValues )
	{
		if ( FUNC::Test ( Span_T<T_COMP> ( (T_COMP*)i.data(), i.size() ), m_iMinValue, m_iMaxValue ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

//////////////////////////////////////////////////////////////////////////

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
class Analyzer_MVA_T : public Analyzer_T<HAVE_MATCHING_BLOCKS>, public Accessor_MVA_T<T>
{
	using ANALYZER = Analyzer_T<HAVE_MATCHING_BLOCKS>;
	using ACCESSOR = Accessor_MVA_T<T>;

public:
				Analyzer_MVA_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings );

	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) final;

private:
	AnalyzerBlock_MVA_Const_c		m_tBlockConst;
	AnalyzerBlock_MVA_Table_c		m_tBlockTable;
	AnalyzerBlock_MVA_Values_c		m_tBlockValues;

	const Filter_t &		m_tSettings;

	typedef int (Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::*ProcessSubblock_fn)( uint32_t * & pRowID, int iSubblockIdInBlock );
	std::array<ProcessSubblock_fn, to_underlying ( MvaPacking_e::TOTAL )> m_dProcessingFuncs;
	ProcessSubblock_fn				m_fnProcessSubblock = nullptr;

	void		SetupPackingFuncs();

	int			ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockTable ( uint32_t * & pRowID, int iSubblockIdInBlock );

	int			ProcessSubblockConstLen_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockDeltaPFOR_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock );

	int			ProcessSubblockConstLen_Values ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockDeltaPFOR_Values ( uint32_t * & pRowID, int iSubblockIdInBlock );

	int			ProcessSubblockConstLen_Range ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockDeltaPFOR_Range ( uint32_t * & pRowID, int iSubblockIdInBlock );

	bool		MoveToBlock ( int iNextBlock ) final;
};

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::Analyzer_MVA_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings )
	: ANALYZER ( tHeader.GetSettings().m_iSubblockSizeMva )
	, ACCESSOR ( tHeader, pReader )
	, m_tBlockConst ( ANALYZER::m_tRowID )
	, m_tBlockTable ( ANALYZER::m_tRowID )
	, m_tBlockValues ( ANALYZER::m_tRowID )
	, m_tSettings ( tSettings )
{
	m_tBlockConst.Setup(m_tSettings);
	m_tBlockTable.Setup(m_tSettings);
	m_tBlockValues.Setup(m_tSettings);

	SetupPackingFuncs();
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
bool Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	if ( ANALYZER::m_iCurSubblock>=ANALYZER::m_iTotalSubblocks )
		return false;

	uint32_t * pRowIdStart = ANALYZER::m_dCollected.data();
	uint32_t * pRowID = pRowIdStart;
	uint32_t * pRowIdMax = pRowIdStart + ACCESSOR::m_iSubblockSize;

	// we scan until we find at least 128 (subblock size) matches.
	// this might lead to this analyzer scanning the whole index
	// a more responsive version would return after processing each 128 docs
	// (even if it doesn't find any matches)
	while ( pRowID<pRowIdMax )
	{
		int iSubblockIdInBlock;
		if ( HAVE_MATCHING_BLOCKS )
			iSubblockIdInBlock = ACCESSOR::GetSubblockIdInBlock ( ANALYZER::m_pMatchingSubblocks->GetBlock ( ANALYZER::m_iCurSubblock ) );
		else
			iSubblockIdInBlock = ACCESSOR::GetSubblockIdInBlock ( ANALYZER::m_iCurSubblock );

		assert(m_fnProcessSubblock);
		ANALYZER::m_iNumProcessed += (*this.*m_fnProcessSubblock) ( pRowID, iSubblockIdInBlock );

		if ( !ANALYZER::MoveToSubblock ( ANALYZER::m_iCurSubblock+1 ) )
			break;
	}

	return CheckEmptySpan ( pRowID, pRowIdStart, dRowIdBlock );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
void Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::SetupPackingFuncs()
{
	auto & dFuncs = m_dProcessingFuncs;
	for ( auto & i : dFuncs )
		i = nullptr;

	// doesn't depend on filter type; just fills result with rowids
	dFuncs [ to_underlying ( MvaPacking_e::CONST ) ] = &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConst;

	// doesn't depend on filter type too; work off pre-calculated array
	dFuncs [ to_underlying ( MvaPacking_e::TABLE ) ] = &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockTable;

	switch ( m_tSettings.m_eType )
	{
	case FilterType_e::VALUES:
		if ( m_tSettings.m_dValues.size()==1 )
		{
			dFuncs [ to_underlying ( MvaPacking_e::CONSTLEN ) ]		= &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConstLen_SingleValue;
			dFuncs [ to_underlying ( MvaPacking_e::DELTA_PFOR ) ]	= &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockDeltaPFOR_SingleValue;
		}
		else
		{
			dFuncs [ to_underlying ( MvaPacking_e::CONSTLEN ) ]		= &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConstLen_Values;
			dFuncs [ to_underlying ( MvaPacking_e::DELTA_PFOR ) ]	= &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockDeltaPFOR_Values;
		}
		break;

	case FilterType_e::RANGE:
		dFuncs [ to_underlying ( MvaPacking_e::CONSTLEN ) ]		= &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConstLen_Range;
		dFuncs [ to_underlying ( MvaPacking_e::DELTA_PFOR ) ]	= &Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockDeltaPFOR_Range;
		break;

	default:
		assert ( 0 && "Unsupported filter type" );
		break;
	}
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	return m_tBlockConst.ProcessSubblock ( pRowID, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock) );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockTable ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockTable.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockTable.ProcessSubblock ( pRowID, ACCESSOR::m_tBlockTable.GetValueIndexes() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConstLen_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockConstLen.ReadSubblock ( iSubblockIdInBlock, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_SingleValue<T,T_COMP,FUNC> ( pRowID, ACCESSOR::m_tBlockConstLen.GetAllValues() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockDeltaPFOR_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_SingleValue<T,T_COMP,FUNC> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConstLen_Values ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockConstLen.ReadSubblock ( iSubblockIdInBlock, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock),*ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_Values<T,T_COMP,FUNC> ( pRowID, ACCESSOR::m_tBlockConstLen.GetAllValues() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockDeltaPFOR_Values ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_Values<T,T_COMP,FUNC> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockConstLen_Range ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockConstLen.ReadSubblock ( iSubblockIdInBlock, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_Range<T,T_COMP,FUNC> ( pRowID, ACCESSOR::m_tBlockConstLen.GetAllValues() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
int Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::ProcessSubblockDeltaPFOR_Range ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_Range<T,T_COMP,FUNC> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template <typename T, typename T_COMP, typename FUNC, bool HAVE_MATCHING_BLOCKS>
bool Analyzer_MVA_T<T,T_COMP,FUNC,HAVE_MATCHING_BLOCKS>::MoveToBlock ( int iNextBlock )
{
	while(true)
	{
		ANALYZER::m_iCurBlockId = iNextBlock;
		ACCESSOR::SetCurBlock ( ANALYZER::m_iCurBlockId );

		if ( ACCESSOR::m_ePacking!=MvaPacking_e::CONST && ACCESSOR::m_ePacking!=MvaPacking_e::TABLE )
			break;

		if ( ACCESSOR::m_ePacking==MvaPacking_e::CONST )
		{
			if ( m_tBlockConst.template SetupNextBlock<T,T_COMP,FUNC> ( ACCESSOR::m_tBlockConst ) )
				break;
		}
		else
		{
			if ( m_tBlockTable.template SetupNextBlock<T,T_COMP,FUNC> ( ACCESSOR::m_tBlockTable ) )
				break;
		}

		while ( iNextBlock==ANALYZER::m_iCurBlockId && ANALYZER::m_iCurSubblock<ANALYZER::m_iTotalSubblocks )
		{
			if ( HAVE_MATCHING_BLOCKS )
				iNextBlock = ACCESSOR::SubblockId2BlockId ( ANALYZER::m_pMatchingSubblocks->GetBlock ( ANALYZER::m_iCurSubblock++ ) );
			else
				iNextBlock = ACCESSOR::SubblockId2BlockId ( ANALYZER::m_iCurSubblock++ );
		}

		if ( ANALYZER::m_iCurSubblock>=ANALYZER::m_iTotalSubblocks )
			return false;
	}

	m_fnProcessSubblock = m_dProcessingFuncs [ to_underlying ( ACCESSOR::m_ePacking ) ];
	assert ( m_fnProcessSubblock );

	return true;
}

//////////////////////////////////////////////////////////////////////////

Iterator_i * CreateIteratorMVA ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
{
	if ( tHeader.GetType()==AttrType_e::UINT32SET )
		return new Iterator_MVA_T<uint32_t> ( tHeader, pReader );

	return new Iterator_MVA_T<uint64_t> ( tHeader, pReader );
}

template <typename ANY, typename ALL>
static Analyzer_i * CreateAnalyzerMVA ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings, bool bHaveMatchingBlocks )
{
	bool b64 = tHeader.GetType()==AttrType_e::INT64SET;
	bool bAny = tSettings.m_eMvaAggr==MvaAggr_e::ANY;
	int iIndex = b64*4 + bAny*2 + bHaveMatchingBlocks;

	switch ( iIndex )
	{
	case 0:		return new Analyzer_MVA_T<uint32_t, uint32_t, ALL, false> ( tHeader, pReader, tSettings );
	case 1:		return new Analyzer_MVA_T<uint32_t, uint32_t, ALL, true> ( tHeader, pReader, tSettings );
	case 2:		return new Analyzer_MVA_T<uint32_t, uint32_t, ANY, false> ( tHeader, pReader, tSettings );
	case 3:		return new Analyzer_MVA_T<uint32_t, uint32_t, ANY, true> ( tHeader, pReader, tSettings );
	case 4:		return new Analyzer_MVA_T<uint64_t, int64_t,  ALL, false> ( tHeader, pReader, tSettings );
	case 5:		return new Analyzer_MVA_T<uint64_t, int64_t,  ALL, true> ( tHeader, pReader, tSettings );
	case 6:		return new Analyzer_MVA_T<uint64_t, int64_t,  ANY, false> ( tHeader, pReader, tSettings );
	case 7:		return new Analyzer_MVA_T<uint64_t, int64_t,  ANY, true> ( tHeader, pReader, tSettings );
	default:	return nullptr;
	}
}

Analyzer_i * CreateAnalyzerMVA ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings, bool bHaveMatchingBlocks )
{
	bool bLeftClosed = tSettings.m_bLeftClosed;
	bool bRightClosed = tSettings.m_bRightClosed;

	int iIndex = bLeftClosed*2 + bRightClosed;

	switch ( iIndex )
	{
	case 0:		return CreateAnalyzerMVA < MvaAny_T<false,false>, MvaAll_T<false,false> > ( tHeader, pReader, tSettings, bHaveMatchingBlocks );
	case 1:		return CreateAnalyzerMVA < MvaAny_T<false,true >, MvaAll_T<false,true > > ( tHeader, pReader, tSettings, bHaveMatchingBlocks );
	case 2:		return CreateAnalyzerMVA < MvaAny_T<true, false>, MvaAll_T<true, false> > ( tHeader, pReader, tSettings, bHaveMatchingBlocks );
	case 3:		return CreateAnalyzerMVA < MvaAny_T<true, true >, MvaAll_T<true, true > > ( tHeader, pReader, tSettings, bHaveMatchingBlocks );
	default:	return nullptr;
	}
}

} // namespace columnar