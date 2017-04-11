#pragma once

struct KTrie;

class KModelMgr
{
protected:
	vector<KForm> forms;
	vector<KMorpheme> morphemes;
#ifdef TRIE_ALLOC_ARRAY
	vector<KTrie> trieRoot;
#else
	shared_ptr<KTrie> trieRoot;
#endif
	//KPOSTag maxiumBf[(size_t)KPOSTag::MAX];
	KPOSTag maxiumBtwn[(size_t)KPOSTag::MAX][(size_t)KPOSTag::MAX];
	KPOSTag maxiumVBtwn[(size_t)KPOSTag::MAX][(size_t)KPOSTag::MAX];
	float posTransition[(size_t)KPOSTag::MAX][(size_t)KPOSTag::MAX];
	void loadPOSFromTxt(const char* filename);
	void savePOSBin(const char* filename) const;
	void loadPOSBin(const char* filename);
	void loadMMFromTxt(const char * filename, unordered_map<string, size_t>& formMap, unordered_map<pair<string, KPOSTag>, size_t>& morphMap);
	void loadCMFromTxt(const char * filename, unordered_map<string, size_t>& formMap, unordered_map<pair<string, KPOSTag>, size_t>& morphMap);
	void loadPCMFromTxt(const char * filename, unordered_map<string, size_t>& formMap, unordered_map<pair<string, KPOSTag>, size_t>& morphMap);
	void saveMorphBin(const char* filename) const;
	void loadMorphBin(const char* filename);
public:
	KModelMgr(const char* posFile = nullptr, const char* morphemeFile = nullptr, const char* combinedFile = nullptr, const char* precombinedFile = nullptr);
	void solidify();
#ifdef TRIE_ALLOC_ARRAY
	const KTrie* getTrie() const { return &trieRoot[0]; }
#else
	const KTrie* getTrie() const { return trieRoot.get(); }
#endif
	float getTransitionP(const KMorpheme* a, const KMorpheme* b) const;
	float getTransitionP(KPOSTag a, KPOSTag b) const;
	//KPOSTag findMaxiumTag(const KMorpheme* b) const;
	KPOSTag findMaxiumTag(const KMorpheme* a, const KMorpheme* c) const;
	float getMaxInOpend(KPOSTag a);
};

