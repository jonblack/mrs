#pragma once

#include <set>

#include <zeep/server.hpp>
#include "M6Server.h"

namespace WSSearchNS
{

struct FileInfo
{
	std::string					id;
	std::string					uuid;
	std::string					version;
	std::string					path;
	std::string					modificationDate;
	uint32						entries;
	uint64						fileSize;
	uint64						rawDataSize;

	template<class Archive>
	void serialize(Archive& ar, const unsigned int)
	{
		ar & BOOST_SERIALIZATION_NVP(id)
		   & BOOST_SERIALIZATION_NVP(uuid)
		   & BOOST_SERIALIZATION_NVP(version)
		   & BOOST_SERIALIZATION_NVP(path)
		   & BOOST_SERIALIZATION_NVP(modificationDate)
		   & BOOST_SERIALIZATION_NVP(entries)
		   & BOOST_SERIALIZATION_NVP(fileSize)
		   & BOOST_SERIALIZATION_NVP(rawDataSize);
	}
};

struct DatabankInfo
{
	std::string					id;
	std::string					name;
	std::string					url;
	std::string					script;
	bool						blastable;
	std::vector<FileInfo>		files;
	std::vector<std::string>	links;
	
	template<class Archive>
	void serialize(Archive& ar, const unsigned int)
	{
		ar & BOOST_SERIALIZATION_NVP(id)
		   & BOOST_SERIALIZATION_NVP(name)
		   & BOOST_SERIALIZATION_NVP(url)
		   & BOOST_SERIALIZATION_NVP(script)
		   & BOOST_SERIALIZATION_NVP(blastable)
		   & BOOST_SERIALIZATION_NVP(files)
		   & BOOST_SERIALIZATION_NVP(links);
	}

};

enum Format
{
	plain,
	title,
	html,
	fasta,
	sequence
};

enum IndexType
{
	Unique,
	FullText,
	Number,
	Date
};

struct Index
{
	std::string					id;
	std::string					description;
	uint32						count;
	IndexType					type;

	template<class Archive>
	void serialize(Archive& ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(id)
		   & BOOST_SERIALIZATION_NVP(description)
		   & BOOST_SERIALIZATION_NVP(count)
		   & BOOST_SERIALIZATION_NVP(type);
	}
};

enum BooleanQueryOperation
{
	CONTAINS,
	LT, LE, EQ, GT, GE,
	UNION, INTERSECTION, NOT,
	ADJACENT,
	CONTAINSSTRING
};

struct BooleanQuery
{
	BooleanQueryOperation		operation;
	std::string					index;
	std::string					value;
	std::vector<BooleanQuery>	leafs;

	template<class Archive>
	void serialize(Archive& ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(operation)
		   & BOOST_SERIALIZATION_NVP(index)
		   & BOOST_SERIALIZATION_NVP(value)
		   & BOOST_SERIALIZATION_NVP(leafs);
	}
};

struct Hit
{
	std::string					id;
	std::string					title;
	float						score;

	template<class Archive>
	void serialize(Archive& ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(id)
		   & BOOST_SERIALIZATION_NVP(title)
		   & BOOST_SERIALIZATION_NVP(score);
	}
};

enum Algorithm
{
	Vector,
	Dice,
	Jaccard
};

struct FindResult
{
	std::string					db;
	uint32						count;
	std::vector<Hit>			hits;

	template<class Archive>
	void serialize(Archive& ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(db)
		   & BOOST_SERIALIZATION_NVP(count)
		   & BOOST_SERIALIZATION_NVP(hits);
	}
};

//struct Cluster
//{
//	std::string					id;
//	std::string					title;
//	float						similarity;
//	std::vector<Cluster>		children;
//
//	template<class Archive>
//	void serialize(Archive& ar, const unsigned int)
//	{
//		ar & BOOST_SERIALIZATION_NVP(id)
//		   & BOOST_SERIALIZATION_NVP(title)
//		   & BOOST_SERIALIZATION_NVP(similarity)
//		   & BOOST_SERIALIZATION_NVP(children);
//	}
//};

}

class M6WSSearch : public zeep::server, public M6SearchServer
{
  public:
					M6WSSearch(const zeep::xml::element* inConfig);

	void			GetDatabankInfo(const std::string& db,
						std::vector<WSSearchNS::DatabankInfo>& info);
	
	void			GetEntry(const std::string& db, const std::string& id,
						WSSearchNS::Format format, std::string& entry);

	void			GetEntryLinesMatchingRegularExpression(const std::string& db,
						const std::string& id, const std::string& regularExpression,
						std::string& text);

	void			GetMetaData(const std::string& db, const std::string& id,
						const std::string& meta, std::string& data);

	void			GetIndices(const std::string& db, std::vector<WSSearchNS::Index>& indices);

	void			Find(const std::string& db, const std::vector<std::string>& queryterms,
						WSSearchNS::Algorithm algorithm, bool alltermsrequired,
						const std::string& booleanfilter, int resultoffset,
						int maxresultcount, std::vector<WSSearchNS::FindResult>& response);

	void			FindBoolean(const std::string& db, const WSSearchNS::BooleanQuery& query,
						int resultoffset, int maxresultcount,
						std::vector<WSSearchNS::FindResult>& response);

//	void			FindSimilar(const std::string& db, const std::string& id,
//						WSSearchNS::Algorithm algorithm, int resultoffset, int maxresultcount,
//						std::vector<WSSearchNS::FindResult>& response);

	void			GetLinked(const std::string& db, const std::string& id,
						const std::string& linkedDb, int resultoffset, int maxresultcount,
						std::vector<WSSearchNS::FindResult>& response);

//	void			Count(const std::string& db, const std::string& booleanquery, uint32& response);
//
//	void			Cooccurrence(const std::string& db, const std::vector<std::string>& ids,
//						float idf_cutoff, int resultoffset, int maxresultcount,
//						std::vector<std::string>& terms);
//
//	void			SpellCheck(const std::string& db, const std::string& queryterm,
//						std::vector<std::string>& suggestions);
//
//	void			SuggestSearchTerms(const std::string& db, const std::string& queryterm,
//						std::vector<std::string>& suggestions);
//
//	void			CompareDocuments(const std::string& db,
//						const std::string& doc_a, const std::string& doc_b, float& similarity);
//
//	void			ClusterDocuments(const std::string& db, const std::vector<std::string>& ids,
//						WSSearchNS::Cluster& response);
};