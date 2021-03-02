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

#ifndef _columnar_
#define _columnar_

#include "util/util.h"
#include <functional>

namespace columnar
{

static const int LIB_VERSION = 4;

class Iterator_i
{
public:
	virtual				~Iterator_i() = default;

	virtual	uint32_t	AdvanceTo ( uint32_t tRowID ) = 0;

	virtual	int64_t		Get() = 0;

	virtual	int			Get ( const uint8_t * & pData, bool bPack ) = 0;
	virtual	int			GetLength() const = 0;

	virtual uint64_t	GetStringHash() = 0;
	virtual bool		HaveStringHashes() const = 0;
};


class BlockIterator_i
{
public:
	virtual				~BlockIterator_i() = default;

	virtual bool		HintRowID ( uint32_t tRowID ) = 0;
	virtual bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) = 0;
	virtual int64_t		GetNumProcessed() const = 0;
};


using MinMaxVec_t = std::vector<std::pair<int64_t,int64_t>>;

class BlockTester_i
{
public:
	virtual				~BlockTester_i() = default;

	virtual bool		Test ( const MinMaxVec_t & dMinMax ) const = 0;
};


struct IteratorHints_t
{
	bool	m_bSequential = false;
	bool	m_bNeedStringHashes = false;
};


enum class FilterType_e
{
	NONE,
	VALUES,
	RANGE,
	FLOATRANGE
};


enum class MvaAggr_e
{
	NONE,
	ALL,
	ANY
};


struct Filter_t
{
	std::string				m_sName;
	bool					m_bExclude = false;
	FilterType_e			m_eType = FilterType_e::NONE;
	MvaAggr_e				m_eMvaAggr = MvaAggr_e::NONE;
	int64_t					m_iMinValue = 0;
	int64_t					m_iMaxValue = 0;
	float					m_fMinValue = 0.0f;
	float					m_fMaxValue = 0.0f;
	bool					m_bLeftUnbounded = false;
	bool					m_bRightUnbounded = false;
	bool					m_bLeftClosed = true;
	bool					m_bRightClosed = true;

	std::vector<int64_t>	m_dValues;
};

enum class AttrType_e : uint32_t
{
	NONE,
	UINT32,
	TIMESTAMP,
	INT64,
	BOOLEAN,
	FLOAT,
	STRING,
	UINT32SET,
	INT64SET
};

class FileReader_c;

struct Settings_t
{
	int			m_iSubblockSize = 128;
	int			m_iSubblockSizeMva = 128;
	int			m_iMinMaxLeafSize = 128;
	std::string	m_sCompressionUINT32 = "simdfastpfor128";
	std::string	m_sCompressionUINT64 = "fastpfor128";

	void		Load ( FileReader_c & tReader );
	void		Save ( FileWriter_c & tWriter );
};

using GetAttrId_fn = std::function<int(std::string)>;

class Columnar_i
{
public:
	virtual					~Columnar_i() = default;

	virtual Iterator_i *	CreateIterator ( const std::string & sName, const IteratorHints_t & tHints, std::string & sError ) const = 0;
	virtual std::vector<BlockIterator_i *> CreateAnalyzerOrPrefilter ( const std::vector<Filter_t> & dFilters, std::vector<int> & dDeletedFilters, const BlockTester_i & tBlockTester, const GetAttrId_fn & fnGetAttrId ) const = 0;

	virtual bool			EarlyReject ( const std::vector<Filter_t> & dFilters, const BlockTester_i & tBlockTester, const GetAttrId_fn & fnGetAttrId ) const = 0;
	virtual bool			IsFilterDegenerate ( const Filter_t & tFilter ) const = 0;
};

} // namespace columnar


extern "C"
{
	DLLEXPORT columnar::Columnar_i *	CreateColumnarStorageReader ( const std::string & sFilename, uint32_t uTotalDocs, std::string & sError );
	DLLEXPORT void						SetupColumnar ( columnar::Malloc_fn fnMalloc, columnar::Free_fn fnFree );
	DLLEXPORT int						GetColumnarLibVersion();
}

#endif // _columnar_