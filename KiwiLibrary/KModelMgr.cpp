#include "stdafx.h"
#include "KForm.h"
#include "KTrie.h"
#include "Utils.h"
#include "KModelMgr.h"

namespace std
{
	template <>
	class hash<pair<k_string, KPOSTag>> {
	public:
		size_t operator() (const pair<k_string, KPOSTag>& o) const
		{
			return hash<k_string>{}(o.first) ^ (size_t)o.second;
		};
	};
}

using namespace std;

//#define LOAD_TXT

void KModelMgr::loadMMFromTxt(std::istream& is, morphemeMap& morphMap)
{
#ifdef LOAD_TXT
	// preserve places for default tag forms & morphemes
	forms.resize((size_t)KPOSTag::SN); 
	morphemes.resize((size_t)KPOSTag::SN + 2); // additional places for <s> & </s>
	for (size_t i = 0; i < (size_t)KPOSTag::SN; ++i)
	{
		forms[i].candidate.emplace_back((KMorpheme*)(i + 2));
	}

	string line;
	while (getline(is, line))
	{
		auto wstr = utf8_to_utf16(line);
		if (wstr.back() == '\n') wstr.pop_back();
		auto fields = split(wstr, u'\t');
		if (fields.size() < 8) continue;

		auto form = normalizeHangul({ fields[0].begin(), fields[0].end() });
		auto tag = makePOSTag({ fields[1].begin(), fields[1].end() });
		float morphWeight = stof(fields[2].begin(), fields[2].end());
		if (morphWeight < 10 && tag >= KPOSTag::JKS)
		{
			continue;
		}
		float vowel = stof(fields[4].begin(), fields[4].end());
		float vocalic = stof(fields[5].begin(), fields[5].end());
		float vocalicH = stof(fields[6].begin(), fields[6].end());
		float positive = stof(fields[7].begin(), fields[7].end());

		KCondVowel cvowel = KCondVowel::none;
		KCondPolarity polar = KCondPolarity::none;
		if (tag >= KPOSTag::JKS && tag <= KPOSTag::ETM)
		{
			float t[] = { vowel, vocalic, vocalicH, 1 - vowel, 1 - vocalic, 1 - vocalicH };
			size_t pmIdx = max_element(t, t + LEN_ARRAY(t)) - t;
			if (t[pmIdx] >= 0.825f)
			{
				cvowel = (KCondVowel)(pmIdx + 2);
			}
			else
			{
				cvowel = KCondVowel::any;
			}

			float u[] = { positive, 1 - positive };
			pmIdx = max_element(u, u + 2) - u;
			if (u[pmIdx] >= 0.825f)
			{
				polar = (KCondPolarity)(pmIdx + 1);
			}
		}
		size_t mid = morphemes.size();
		morphMap.emplace(make_pair(form, tag), mid);
		auto& fm = formMapper(form);
		fm.candidate.emplace_back((KMorpheme*)mid);
		fm.suffix.insert(0);
		morphemes.emplace_back(form, tag, cvowel, polar);
		morphemes.back().kform = (const k_string*)(&fm - &forms[0]);
	}
#endif
}


void KModelMgr::loadCMFromTxt(std::istream& is, morphemeMap& morphMap)
{
#ifdef LOAD_TXT
	static k_char* conds[] = { KSTR("+"), KSTR("-Coda"), KSTR("+"), KSTR("+") };
	static k_char* conds2[] = { KSTR("+"), KSTR("+Positive"), KSTR("-Positive") };

	string line;
	while (getline(is, line))
	{
		auto wstr = utf8_to_utf16(line);
		if (wstr.back() == '\n') wstr.pop_back();
		auto fields = split(wstr, u'\t');
		if (fields.size() < 2) continue;
		if (fields.size() == 2) fields.emplace_back();
		auto form = normalizeHangul({ fields[0].begin(), fields[0].end() });
		vector<const KMorpheme*>* chunkIds = new vector<const KMorpheme*>;
		float ps = 0;
		size_t bTag = 0;
		for (auto chunk : split(fields[1], u'+'))
		{
			auto c = split(chunk, u'/');
			if (c.size() < 2) continue;
			auto f = normalizeHangul({ c[0].begin(), c[0].end() });
			auto tag = makePOSTag({ c[1].begin(), c[1].end() });
			auto it = morphMap.find(make_pair(f, tag));
			if (it != morphMap.end())
			{
				chunkIds->emplace_back((KMorpheme*)it->second);
			}
			else
			{
				goto continueFor;
			}
			bTag = (size_t)tag;
		}

		KCondVowel vowel = morphemes[((size_t)chunkIds->at(0))].vowel;
		KCondPolarity polar = morphemes[((size_t)chunkIds->at(0))].polar;
		auto pm = find(conds, conds + LEN_ARRAY(conds), fields[2]);
		if (pm < conds + LEN_ARRAY(conds))
		{
			vowel = (KCondVowel)(pm - conds + 1);
		}
		pm = find(conds2, conds2 + LEN_ARRAY(conds2), fields[2]);
		if (pm < conds2 + LEN_ARRAY(conds2))
		{
			polar = (KCondPolarity)(pm - conds2);
		}
		uint8_t combineSocket = 0;
		if (fields.size() >= 4 && !fields[3].empty())
		{
			combineSocket = (size_t)stof(fields[3].begin(), fields[3].end());
		}

		size_t mid = morphemes.size();
		auto& fm = formMapper(form);
		fm.candidate.emplace_back((KMorpheme*)mid);
		//fm.suffix.insert(0);
		morphemes.emplace_back(form, KPOSTag::UNKNOWN, vowel, polar, combineSocket);
		morphemes.back().kform = (const k_string*)(&fm - &forms[0]);
		morphemes.back().chunks = chunkIds;
	continueFor:;
	}
#endif
}


void KModelMgr::loadPCMFromTxt(std::istream& is, morphemeMap& morphMap)
{
#ifdef LOAD_TXT
	string line;
	while (getline(is, line))
	{
		auto wstr = utf8_to_utf16(line);
		if (wstr.back() == '\n') wstr.pop_back();
		auto fields = split(wstr, u'\t');
		if (fields.size() < 4) continue;

		auto combs = split(fields[0], u'+');
		auto org = combs[0] + combs[1];
		auto form = normalizeHangul({ combs[0].begin(), combs[0].end() });
		auto orgform = normalizeHangul({ org.begin(), org.end() });
		auto tag = makePOSTag({ fields[1].begin(), fields[1].end() });
		k_string suffixes = normalizeHangul({ fields[2].begin(), fields[2].end() });
		uint8_t socket = (size_t)stof(fields[3].begin(), fields[3].end());

		auto mit = morphMap.find(make_pair(orgform, tag));
		assert(mit != morphMap.end());
		if (!form.empty())
		{
			size_t mid = morphemes.size();
			//morphMap.emplace(make_pair(form, tag), mid);
			auto& fm = formMapper(form);
			fm.candidate.emplace_back((KMorpheme*)mid);
			fm.suffix.insert(suffixes.begin(), suffixes.end());
			morphemes.emplace_back(form, tag, KCondVowel::none, KCondPolarity::none, socket);
			morphemes.back().kform = (const k_string*)(&fm - &forms[0]);
			morphemes.back().combined = (int)mit->second - ((int)morphemes.size() - 1);
		}
	}
#endif
}

void KModelMgr::loadCorpusFromTxt(std::istream & is, morphemeMap& morphMap)
{
	string line;
	vector<uint32_t> wids;
	wids.emplace_back(0);
	while (getline(is, line))
	{
		auto wstr = utf8_to_utf16(line);
		if (wstr.back() == '\n') wstr.pop_back();
		if (wstr.empty() && wids.size() > 1)
		{
			wids.emplace_back(1);
			langMdl.trainSequence(&wids[0], wids.size());
			wids.erase(wids.begin() + 1, wids.end());
			continue;
		}
		auto fields = split(wstr, u'\t');
		if (fields.size() < 2) continue;

		for (size_t i = 1; i < fields.size(); i += 2)
		{
			auto f = normalizeHangul(fields[i]);
			auto t = makePOSTag(fields[i + 1]);

			if (f.empty()) continue;
			if ((f[0] == u'��' || f[0] == u'��') && fields[i + 1][0] == 'E')
			{
				if(f[0] == u'��') f[0] == u'��';
				else f[0] == u'��';
			}
			
			auto it = morphMap.find(make_pair(f, t));
			if (it == morphMap.end() || morphemes[it->second].chunks || morphemes[it->second].combineSocket)
			{
				if (t <= KPOSTag::SN && t != KPOSTag::UNKNOWN)
				{
					wids.emplace_back((size_t)t + 1);
				}
				else
				{
					wids.emplace_back((size_t)KPOSTag::NNP);
				}
			}
			else
			{
				wids.emplace_back(it->second);
			}
		}
	}
}

void KModelMgr::saveMorphBin(std::ostream& os) const
{
	writeToBinStream(os, 0x4B495749);
	writeToBinStream<uint32_t>(os, forms.size());
	writeToBinStream<uint32_t>(os, morphemes.size());

	auto mapper = [this](const KMorpheme* p)->size_t
	{
		return (size_t)p;
	};

	for (const auto& form : forms)
	{
		form.writeToBin(os, mapper);
	}
	for (const auto& morph : morphemes)
	{
		morph.writeToBin(os, mapper);
	}
}

void KModelMgr::loadMorphBin(std::istream& is)
{
	if (readFromBinStream<uint32_t>(is) != 0x4B495749) throw exception();
	size_t formSize = readFromBinStream<uint32_t>(is);
	size_t morphemeSize = readFromBinStream<uint32_t>(is);
	
	forms.resize(formSize);
	morphemes.resize(morphemeSize);

	auto mapper = [this](size_t p)->const KMorpheme*
	{
		return (const KMorpheme*)p;
	};

	for (auto& form : forms)
	{
		form.readFromBin(is, mapper);
		formMap.emplace(form.form, formMap.size());
	}
	for (auto& morph : morphemes)
	{
		morph.readFromBin(is, mapper);
	}
}

KForm & KModelMgr::formMapper(k_string form)
{
	auto it = formMap.find(form);
	if (it != formMap.end()) return forms[it->second];
	size_t id = forms.size();
	formMap.emplace(form, id);
	forms.emplace_back(form);
	return forms[id];
}

KModelMgr::KModelMgr(const char * modelPath) : langMdl(3)
{
	this->modelPath = modelPath;
#ifdef LOAD_TXT
	unordered_map<pair<k_string, KPOSTag>, size_t> morphMap;
	loadMMFromTxt(ifstream{ modelPath + string{ "fullmodelV2.txt" } }, morphMap);
	loadCMFromTxt(ifstream{ modelPath + string{ "combinedV2.txt" } }, morphMap);
	loadPCMFromTxt(ifstream{ modelPath + string{ "precombinedV2.txt" } }, morphMap);
	saveMorphBin(ofstream{ modelPath + string{ "morpheme.bin" }, ios_base::binary });

	loadCorpusFromTxt(ifstream{ modelPath + string{ "ML_lit.txt" } }, morphMap);
	loadCorpusFromTxt(ifstream{ modelPath + string{ "ML_spo.txt" } }, morphMap);
	langMdl.optimize();
	langMdl.writeToStream(ofstream{ modelPath + string{ "langMdl.bin" }, ios_base::binary });
#else
	loadMorphBin(ifstream{ modelPath + string{"morpheme.bin"}, ios_base::binary });
	langMdl = KNLangModel::readFromStream(ifstream{ modelPath + string{ "langMdl.bin" }, ios_base::binary });
#endif
}

void KModelMgr::addUserWord(const k_string & form, KPOSTag tag)
{
#ifdef TRIE_ALLOC_ARRAY
	if (!form.empty()) extraTrieSize += form.size() - 1;
#else
#endif

	auto& f = formMapper(form);
	f.candidate.emplace_back((const KMorpheme*)morphemes.size());
	morphemes.emplace_back(form, tag);
}

void KModelMgr::addUserRule(const k_string & form, const vector<pair<k_string, KPOSTag>>& morphs)
{
#ifdef TRIE_ALLOC_ARRAY
	if (!form.empty()) extraTrieSize += form.size() - 1;
#else
#endif

	auto& f = formMapper(form);
	f.candidate.emplace_back((const KMorpheme*)morphemes.size());
	morphemes.emplace_back(form, KPOSTag::UNKNOWN);
	morphemes.back().chunks = new vector<const KMorpheme*>(morphs.size());
	iota(morphemes.back().chunks->begin(), morphemes.back().chunks->end(), (const KMorpheme*)morphemes.size());
	for (auto& m : morphs)
	{
		morphemes.emplace_back(m.first, m.second);
	}
}

void KModelMgr::solidify()
{
#ifdef TRIE_ALLOC_ARRAY
	trieRoot.reserve(100000 + extraTrieSize);
	trieRoot.resize((size_t)KPOSTag::SN + 1); // preserve places for root node + default tag morphemes
	for (size_t i = 1; i <= (size_t)KPOSTag::SN; ++i)
	{
		trieRoot[i].val = &forms[i - 1];
	}

	for (size_t i = (size_t)KPOSTag::SN + 1; i < forms.size(); ++i)
	{
		auto& f = forms[i];
		if (f.candidate.empty()) continue;
		trieRoot[0].build(&f.form[0], f.form.size(), &f, [this]()
		{
			trieRoot.emplace_back();
			return &trieRoot.back();
		});
	}
	trieRoot[0].fillFail();
#else
	trieRoot = make_shared<KTrie>();
	for (auto& f : forms)
	{
		trieRoot->build(f.form.c_str(), &f);
	}
	trieRoot->fillFail();
#endif

	for (auto& f : morphemes)
	{
		f.kform = &forms[(size_t)f.kform].form;
		if (f.chunks) for (auto& p : *f.chunks) p = &morphemes[(size_t)p];
	}

	for (auto& f : forms)
	{
		for (auto& p : f.candidate) p = &morphemes[(size_t)p];
	}
	formMap = {};
}
