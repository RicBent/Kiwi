#pragma once

#include <cmath>
#include <iostream>
#include <Eigen/Dense>
#include <kiwi/ThreadPool.h>
#include <kiwi/SkipBigramModel.h>
#include "RaggedVector.hpp"
#include "serializerEigen.hpp"
#include "BitEncoder.hpp"

namespace kiwi
{
	namespace sb
	{
		struct TrainContext
		{
			Eigen::ArrayXf grad;
			Vector<uint8_t> updatedMask;
		};

		template<class VocabTy>
		struct FeedingData
		{
			const VocabTy* x = nullptr;
			const float* lmLogProbs = nullptr;
			size_t len = 0;
		};

		struct ObservingData
		{
			size_t prevGlobalStep = 0;
			size_t globalStep = 0;
			float llMeanTotal = 0;
			float llRecent = 0;
			size_t cntTotal = 0;
			size_t cntRecent = 0;
		};

		/**
		* calculate log(exp(a) + exp(b))
		*/
		template<class A, class B>
		auto logAddExp(const Eigen::ArrayBase<A>& a, const Eigen::ArrayBase<B>& b)
			-> decltype(((a.min(b) - a.max(b)).exp() + 1).log() + a.max(b))
		{
			return ((a.min(b) - a.max(b)).exp() + 1).log() + a.max(b);
		}

		/**
		* calculate log(exp(a).sum(axis=0))
		*/
		template<class A>
		auto logSumExp(const Eigen::ArrayBase<A>& a)
			-> decltype((a.rowwise() - a.colwise().maxCoeff()).exp().colwise().sum().log() + a.colwise().maxCoeff())
		{
			return (a.rowwise() - a.colwise().maxCoeff()).exp().colwise().sum().log() + a.colwise().maxCoeff();
		}

		/**
		* P(W | X_1, X_2 ... X_w) := mean_i P(W | X_i)
		*
		* SkipBigramTrainer Class tries to find logits that maximize P(W | X_1, X_2 ... X_w)
		*
		* LM(W | X) := probabilities from base language model
		* P(W | X_i) := softmax_W(Theta_X_i) + LM(W | X_i) * softmax_O(Theta_X)
		*/
		template<class VocabTy, size_t windowSize>
		class SkipBigramTrainer
		{
			size_t bosToken = 0;
			Vector<size_t> ptrs;
			Vector<VocabTy> vocabs;
			Eigen::ArrayXf logits;
			Vector<uint8_t> vocabValidness;
			bool skipEmptyWords = false;

			static constexpr size_t gradientBlockSize = 128;
			static constexpr float lmInitialBias = 10;
			float lmRegularizingLimit = 0.333;

		private:

			/**
			* log P(W|X) = log(mean_i (softmax_W(Theta_X_i) + softmax_O(Theta_X_i) * LM(W|X)) )
			*
			* grad(log P(w|X), Theta_X_t) = grad(P(w|X), Theta_X_t) / P(w|X)
			*   = ( softmax_w(Theta_X_t) * (I_{w} - softmax_w(Theta_X_t)) + softmax_o(Theta_X_t)(I_{o} - softmax_o(Theta_X_t)) * LM(W|X) )
			*     / ( sum_i (softmax_w(Theta_X_i) + softmax_o(Theta_X_i) * LM(W|X)) )
			*
			* assert x[0] == bosToken and x[len - 1] == eosToken
			*/
			float accumulateGradientExceptEmptyWords(const VocabTy* x, const float* lmLogProbs, size_t len, Eigen::ArrayXf& grad, Vector<uint8_t>& updatedMask) const
			{
				float ret = 0;
				Vector<VocabTy> validX;
				Vector<float> validLProbs;
				validX.emplace_back(x[0]);
				validLProbs.emplace_back(lmLogProbs[0]);
				for (size_t i = 1; i < len - 1; ++i)
				{
					if (vocabValidness[x[i]])
					{
						validX.emplace_back(x[i]);
						validLProbs.emplace_back(lmLogProbs[i]);
					}
					else
					{
						ret += lmLogProbs[i];
					}
				}
				validX.emplace_back(x[len - 1]);
				validLProbs.emplace_back(lmLogProbs[len - 1]);

				if (validX.size() <= 2)
				{
					return ret + std::accumulate(validLProbs.begin() + 1, validLProbs.end(), 0.f);
				}
				else
				{
					return ret + accumulateGradient(validX.data(), validLProbs.data(), validX.size(), grad, updatedMask);
				}
			}

			float accumulateGradient(const VocabTy* x, const float* lmLogProbs, size_t len, Eigen::ArrayXf& grad, Vector<uint8_t>& updatedMask) const
			{
				const size_t wOffset = skipEmptyWords ? 0 : 1;

				Vector<size_t> bufPtrs(len - 1 - wOffset);
				Vector<VocabTy> bufIdcs(windowSize * (len - 2 - wOffset));
				size_t maxNnz = 0;
				for (size_t i = 1; i < len - 1 - wOffset; ++i)
				{
					size_t nnz = ptrs[x[i] + 1] - ptrs[x[i]];
					maxNnz = std::max(nnz, maxNnz);
					bufPtrs[i] = bufPtrs[i - 1] + (nnz > 1 ? nnz : 0);
				}
				Eigen::ArrayXf lsBuf{ bufPtrs.back() }, onehotBuf = Eigen::ArrayXf::Zero(maxNnz);
				Eigen::ArrayXXf llBuf{ windowSize * 2, (len - wOffset) };
				llBuf.fill(-INFINITY);
				auto llModel = llBuf.template topRows<windowSize>();
				auto llBase = llBuf.template bottomRows<windowSize>();

				{
					size_t b = ptrs[bosToken], e = ptrs[bosToken + 1];
					for (size_t j = 0; j <= std::min(windowSize, len - 2); ++j)
					{
						auto target = x[j + 1];
						llModel.block(0, j, windowSize + 1 - (j ? j : 1), 1).fill(lmLogProbs[j + 1]);
					}
				}

				size_t s = 0;
				for (size_t i = 1; i < len - 1 - wOffset; ++i)
				{
					auto cond = x[i];
					size_t b = ptrs[cond], e = ptrs[cond + 1];
					size_t segSize = e - b;
					if (segSize > 1)
					{
						auto segment = logits.segment(b, segSize);
						auto buf = lsBuf.segment(bufPtrs[i - 1], bufPtrs[i] - bufPtrs[i - 1]);
						float logsumSegment = std::log((segment - segment.maxCoeff()).exp().sum());
						buf = segment - segment.maxCoeff() - logsumSegment;

						for (size_t j = 1; j <= std::min(windowSize, len - i - 1 - wOffset); ++j)
						{
							auto target = x[i + j + wOffset];
							auto it = std::lower_bound(vocabs.data() + b, vocabs.data() + e - 1, target);
							if (it != vocabs.data() + e - 1 && *it == target)
							{
								size_t idx = it - (vocabs.data() + b);
								bufIdcs[s++] = idx;
								llModel(windowSize - j, i + j) = buf[idx];
							}
							else
							{
								bufIdcs[s++] = segSize - 1;
							}
							llBase(windowSize - j, i + j) = buf[segSize - 1] + lmLogProbs[i + j + wOffset];
						}
					}
					else
					{
						for (size_t j = 1; j <= std::min(windowSize, len - i - 1 - wOffset); ++j)
						{
							auto target = x[i + j + wOffset];
							llBase(windowSize - j, i + j) = lmLogProbs[i + j + wOffset];
						}
					}
				}

				auto sBuf = lsBuf.exp().eval();
				auto logProbSum = logSumExp(llBuf).eval();
				auto denom = logProbSum.exp().eval();

				s = 0;
				for (size_t i = 1; i < len - 1 - wOffset; ++i)
				{
					auto cond = x[i];
					size_t b = ptrs[cond], e = ptrs[cond + 1];
					size_t segSize = e - b;
					if (segSize <= 1) continue;

					auto buf = sBuf.segment(bufPtrs[i - 1], bufPtrs[i] - bufPtrs[i - 1]);
					auto gradSegment = grad.segment(b, segSize);
					auto onehot = onehotBuf.head(segSize);

					size_t gb = b / gradientBlockSize;
					size_t ge = (e + gradientBlockSize - 1) / gradientBlockSize;
					std::fill(updatedMask.begin() + gb, updatedMask.begin() + ge, 0xFF);
					for (size_t j = 1; j <= std::min(windowSize, len - i - 1 - wOffset); ++j)
					{
						auto target = x[i + j + wOffset];
						size_t idx = bufIdcs[s++];
						if (idx < segSize - 1)
						{
							onehot[idx] = 1;
							gradSegment += (buf[idx] / denom[i + j]) * (onehot - buf);
							onehot[idx] = 0;
						}
						onehot[segSize - 1] = 1;
						gradSegment += (buf[segSize - 1] / denom[i + j] * std::exp(lmLogProbs[i + j + wOffset])) * (onehot - buf);
						onehot[segSize - 1] = 0;
					}

					if (buf[segSize - 1] < lmRegularizingLimit)
					{
						onehot[segSize - 1] = 1;
						gradSegment += (onehot - buf) * (lmRegularizingLimit - buf[segSize - 1]) / lmRegularizingLimit;
						onehot[segSize - 1] = 0;
					}
				}

				return (logProbSum - std::log((float)windowSize)).sum();
			}

			float evaluateExceptEmptyWords(const VocabTy* x, const float* lmLogProbs, size_t len) const
			{
				float ret = 0;
				Vector<VocabTy> validX;
				Vector<float> validLProbs;
				validX.emplace_back(x[0]);
				validLProbs.emplace_back(lmLogProbs[0]);
				for (size_t i = 1; i < len - 1; ++i)
				{
					if (vocabValidness[x[i]])
					{
						validX.emplace_back(x[i]);
						validLProbs.emplace_back(lmLogProbs[i]);
					}
					else
					{
						ret += lmLogProbs[i];
					}
				}
				validX.emplace_back(x[len - 1]);
				validLProbs.emplace_back(lmLogProbs[len - 1]);

				if (validX.size() <= 2)
				{
					return ret + std::accumulate(validLProbs.begin() + 1, validLProbs.end(), 0.f);
				}
				else
				{
					return ret + evaluateAll(validX.data(), validLProbs.data(), validX.size());
				}
			}

			/**
			* assert x[0] == bosToken and x[len - 1] == eosToken
			*/
			float evaluateAll(const VocabTy* x, const float* lmLogProbs, size_t len) const
			{
				const size_t wOffset = skipEmptyWords ? 0 : 1;

				Eigen::ArrayXXf llBuf{ windowSize * 2, (len - wOffset) };
				llBuf.fill(-INFINITY);
				auto llModel = llBuf.template topRows<windowSize>();
				auto llBase = llBuf.template bottomRows<windowSize>();

				{
					size_t b = ptrs[bosToken], e = ptrs[bosToken + 1];
					for (size_t j = 0; j <= std::min(windowSize, len - 2); ++j)
					{
						auto target = x[j + 1];
						llModel.block(0, j, windowSize + 1 - (j ? j : 1), 1).fill(lmLogProbs[j + 1]);
					}
				}

				size_t s = 0;
				for (size_t i = 1; i < len - 1 - wOffset; ++i)
				{
					auto cond = x[i];
					size_t b = ptrs[cond], e = ptrs[cond + 1];
					size_t segSize = e - b;
					if (segSize > 1)
					{
						auto segment = logits.segment(b, segSize);
						float logsumSegment = std::log((segment - segment.maxCoeff()).exp().sum());
						auto buf = (segment - segment.maxCoeff() - logsumSegment).eval();

						for (size_t j = 1; j <= std::min(windowSize, len - i - 1 - wOffset); ++j)
						{
							auto target = x[i + j + wOffset];
							auto it = std::lower_bound(vocabs.data() + b, vocabs.data() + e - 1, target);
							if (it != vocabs.data() + e - 1 && *it == target)
							{
								size_t idx = it - (vocabs.data() + b);
								llModel(windowSize - j, i + j) = buf[idx];
							}
							else
							{
							}
							llBase(windowSize - j, i + j) = buf[segSize - 1] + lmLogProbs[i + j + wOffset];
						}
					}
					else
					{
						for (size_t j = 1; j <= std::min(windowSize, len - i - 1 - wOffset); ++j)
						{
							auto target = x[i + j + wOffset];
							llBase(windowSize - j, i + j) = lmLogProbs[i + j + wOffset];
						}
					}
				}
				return (logSumExp(llBuf) - std::log((float)windowSize)).sum();
			}


		public:
			SkipBigramTrainer()
			{
			}

			template<class TokenFilter, class PairFilter>
			SkipBigramTrainer(const RaggedVector<VocabTy>& sents,
				TokenFilter&& tokenFilter,
				PairFilter&& pairFilter,
				VocabTy _bosToken,
				size_t minCnt,
				size_t minCoCnt,
				bool _skipEmptyWords,
				float _lmRegularizingLimit,
				float pmiThreshold = 1,
				size_t maxDataSize = -1)
				: bosToken{ _bosToken }, skipEmptyWords{ _skipEmptyWords }, lmRegularizingLimit{ _lmRegularizingLimit }
			{
				size_t vocabSize = 0;
				UnorderedMap<std::pair<VocabTy, VocabTy>, size_t> pairCounter;

				if (skipEmptyWords)
				{
					Vector<VocabTy> validTokens;
					for (auto r : sents)
					{
						validTokens.clear();
						for (size_t i = 1; i < r.size(); ++i)
						{
							if (tokenFilter(r[i])) validTokens.emplace_back(r[i]);
							vocabSize = vocabSize > r[i] ? vocabSize : r[i];
						}

						for (size_t i = 0; i < validTokens.size(); ++i)
						{
							for (size_t j = 1; j <= windowSize; ++j)
							{
								pairCounter[std::make_pair(j <= i ? validTokens[i - j] : bosToken, validTokens[i])]++;
							}
						}
					}
				}
				else
				{
					for (auto r : sents)
					{
						for (size_t i = 1; i < r.size(); ++i)
						{
							vocabSize = vocabSize > r[i] ? vocabSize : r[i];
							if (!tokenFilter(r[i])) continue;
							for (size_t j = 2; j < windowSize + 2; ++j)
							{
								if (j <= i && !tokenFilter(r[i - j])) continue;
								pairCounter[std::make_pair(j <= i ? r[i - j] : bosToken, r[i])]++;
							}
						}
					}
				}
				vocabSize++;

				vocabValidness.resize(vocabSize);
				for (size_t i = 0; i < vocabSize; ++i)
				{
					vocabValidness[i] = tokenFilter(i) ? 1 : 0;
				}

				size_t dataSize = 0, totCnt = 0;
				Vector<size_t> aCnts(vocabSize), bCnts(vocabSize);
				Vector<Vector<VocabTy>> skipBigramVocabs(vocabSize);
				for (auto& p : pairCounter)
				{
					aCnts[p.first.first] += p.second;
					bCnts[p.first.second] += p.second;
					totCnt += p.second;
				}

				Vector<float> unigramProbs(vocabSize);
				for (size_t i = 0; i < vocabSize; ++i)
				{
					unigramProbs[i] = bCnts[i] / (float)totCnt;
				}

				if (pmiThreshold >= 1)
				{
					Vector<float> pmiValues;
					for (auto& p : pairCounter)
					{
						if (aCnts[p.first.first] < minCnt * windowSize) continue;
						if (bCnts[p.first.second] < minCnt * windowSize) continue;
						if (p.second < minCoCnt) continue;
						if (!pairFilter(p.first.first, p.first.second)) continue;
						float pmi = std::log(p.second / unigramProbs[p.first.second] / aCnts[p.first.first]);
						pmi /= -std::log(p.second / (float)totCnt);
						if (pmi < 0) continue;
						pmiValues.emplace_back(pmi);
					}

					std::sort(pmiValues.begin(), pmiValues.end());
					if (pmiValues.size() < maxDataSize) pmiThreshold = pmiValues[0];
					else pmiThreshold = *(pmiValues.end() - maxDataSize);
				}

				for (auto& p : pairCounter)
				{
					if (aCnts[p.first.first] < minCnt * windowSize) continue;
					if (bCnts[p.first.second] < minCnt * windowSize) continue;
					if (p.second < minCoCnt) continue;
					if (!pairFilter(p.first.first, p.first.second)) continue;
					float pmi = std::log(p.second / unigramProbs[p.first.second] / aCnts[p.first.first]);
					pmi /= -std::log(p.second / (float)totCnt);
					if (pmi < pmiThreshold) continue;
					skipBigramVocabs[p.first.first].emplace_back(p.first.second);
					if (dataSize++ >= maxDataSize) break;
				}

				ptrs.reserve(vocabSize + 1);
				vocabs.reserve(dataSize + vocabSize);
				logits.resize(dataSize + vocabSize);
				logits.setZero();
				ptrs.emplace_back(0);
				for (size_t i = 0; i < vocabSize; ++i)
				{
					auto& v = skipBigramVocabs[i];
					std::sort(v.begin(), v.end());
					vocabs.insert(vocabs.end(), v.begin(), v.end());
					logits[vocabs.size()] = lmInitialBias;
					vocabs.emplace_back(-1);
					ptrs.emplace_back(vocabs.size());
				}
			}

			/**
			* assert x[0] == bosToken and x[len - 1] == eosToken
			*/
			float evaluate(const VocabTy* x, const float* lmLogProbs, size_t len) const
			{
				if (skipEmptyWords) return evaluateExceptEmptyWords(x, lmLogProbs, len);
				else return evaluateAll(x, lmLogProbs, len);
			}

			TrainContext newContext() const
			{
				TrainContext tc;
				tc.grad = Eigen::ArrayXf::Zero(logits.size());
				tc.updatedMask.resize((logits.size() + gradientBlockSize + 1) / gradientBlockSize);
				return tc;
			}

			float update(const VocabTy* x, const float* lmLogProbs, size_t len, float lr, TrainContext& tc)
			{
				float ll;
				if (skipEmptyWords) ll = accumulateGradientExceptEmptyWords(x, lmLogProbs, len, tc.grad, tc.updatedMask);
				else ll = accumulateGradient(x, lmLogProbs, len, tc.grad, tc.updatedMask);
				size_t b = 0;
				for (size_t i = 0; i < tc.updatedMask.size() - 1; ++i, b += gradientBlockSize)
				{
					if (!tc.updatedMask[i]) continue;
					logits.template segment<gradientBlockSize>(b) += tc.grad.template segment<gradientBlockSize>(b) * lr;
					tc.grad.template segment<gradientBlockSize>(b).setZero();
					tc.updatedMask[i] = 0;
				}
				if (tc.updatedMask.back())
				{
					logits.segment(b, logits.size() - b) += tc.grad.segment(b, logits.size() - b) * lr;
					tc.grad.segment(b, logits.size() - b);
					tc.updatedMask.back() = 0;
				}
				return ll;
			}
		
		private:
			void update(float lr, TrainContext& tc, std::mutex* mutex)
			{
				size_t b = 0;
				for (size_t i = 0; i < tc.updatedMask.size() - 1; ++i, b += gradientBlockSize)
				{
					if (!tc.updatedMask[i]) continue;
					
					{
						std::lock_guard<std::mutex> lg{ mutex[i] };
						logits.template segment<gradientBlockSize>(b) += tc.grad.template segment<gradientBlockSize>(b) * lr;
					}
					tc.grad.template segment<gradientBlockSize>(b).setZero();
					tc.updatedMask[i] = 0;
				}
				if (tc.updatedMask.back())
				{
					{
						std::lock_guard<std::mutex> lg{ mutex[tc.updatedMask.size() - 1]};
						logits.segment(b, logits.size() - b) += tc.grad.segment(b, logits.size() - b) * lr;
					}
					tc.grad.segment(b, logits.size() - b).setZero();
					tc.updatedMask.back() = 0;
				}
			}
		
		public:
			void save(std::ostream& ostr) const
			{
				serializer::writeMany(ostr, bosToken, ptrs, vocabs, logits, skipEmptyWords, vocabValidness);
			}

			void load(std::istream& istr)
			{
				serializer::readMany(istr, bosToken, ptrs, vocabs, logits, skipEmptyWords, vocabValidness);
			}

			template<class DataFeeder, class Observer>
			size_t train(DataFeeder&& dataFeeder, Observer&& observer, Vector<size_t> sampleIdcs, size_t totalSteps, float lrStart)
			{
				float llMean = 0;
				size_t llCnt = 0;
				auto tc = newContext();
				std::mt19937_64 rng{ 42 };
				for (size_t steps = 0; steps < totalSteps;)
				{
					std::shuffle(sampleIdcs.begin(), sampleIdcs.end(), rng);
					if (totalSteps - steps < sampleIdcs.size())
					{
						sampleIdcs.erase(sampleIdcs.begin() + totalSteps - steps, sampleIdcs.end());
					}
					for (auto idx : sampleIdcs)
					{
						FeedingData<VocabTy> d = dataFeeder(idx);
						if (!d.len) return steps;
						float lr = lrStart * std::max((totalSteps - steps) / (double)totalSteps, 1e-5);
						float sum = update(d.x, d.lmLogProbs, d.len, lr, tc);
						size_t cnt = d.len - 1;
						llCnt += cnt;
						llMean += (sum - llMean * cnt) / llCnt;
						ObservingData od;
						od.prevGlobalStep = steps;
						od.globalStep = steps + 1;
						od.llMeanTotal = llMean;
						od.cntTotal = llCnt;
						od.llRecent = sum;
						od.cntRecent = cnt;
						observer(od);
						++steps;
					}
				}
				return totalSteps;
			}

			template<class DataFeeder, class Observer>
			size_t trainMulti(const size_t workers, DataFeeder&& dataFeeder, Observer&& observer, Vector<size_t> sampleIdcs, const size_t totalSteps, const float lrStart)
			{
				const size_t items = workers * 1;
				const size_t updateInterval = 16;
				float llMean = 0;
				size_t llCnt = 0;
				utils::ThreadPool pool{ workers };
				Vector<std::future<std::tuple<float, size_t, size_t>>> futures;
				Vector<TrainContext> tc;
				for (size_t i = 0; i < workers; ++i) tc.emplace_back(newContext());
				std::unique_ptr<std::mutex[]> mutex = make_unique<std::mutex[]>(tc.back().updatedMask.size());
				std::mt19937_64 rng{ 42 };

				for (size_t steps = 0; steps < totalSteps;)
				{
					std::shuffle(sampleIdcs.begin(), sampleIdcs.end(), rng);
					if (totalSteps - steps < sampleIdcs.size())
					{
						sampleIdcs.erase(sampleIdcs.begin() + totalSteps - steps, sampleIdcs.end());
					}

					for (size_t t = 0; t < items; ++t)
					{
						futures.emplace_back(pool.enqueue([&, steps](size_t threadId, size_t itemId)
						{
							float localLlMean = 0;
							size_t localLlCnt = 0;
							size_t localSteps = 0;
							for (size_t i = itemId; i < sampleIdcs.size(); i += items, ++localSteps)
							{
								FeedingData<VocabTy> d = dataFeeder(sampleIdcs[i], threadId);
								if (!d.len) break;
								float sum;
								if (skipEmptyWords) sum = accumulateGradientExceptEmptyWords(d.x, d.lmLogProbs, d.len, tc[threadId].grad, tc[threadId].updatedMask);
								else sum = accumulateGradient(d.x, d.lmLogProbs, d.len, tc[threadId].grad, tc[threadId].updatedMask);
								size_t cnt = d.len - 1;
								localLlCnt += cnt;
								localLlMean += (sum - localLlMean * cnt) / localLlCnt;
								if ((localSteps + 1) % updateInterval == 0)
								{
									float lr = lrStart * std::max((totalSteps - (steps + localSteps * items)) / (double)totalSteps, 1e-5);
									update(lr, tc[threadId], mutex.get());
								}

								if (threadId == 0)
								{
									ObservingData od;
									od.prevGlobalStep = steps + localSteps * items;
									od.globalStep = steps + (localSteps + 1) * items;
									od.llMeanTotal = llMean;
									od.cntTotal = llCnt;
									od.llRecent = sum;
									od.cntRecent = cnt;
									observer(od);
								}
							}
							return std::make_tuple(localLlMean, localLlCnt, localSteps);
						}, t));
					}

					for (auto& f : futures)
					{
						auto v = f.get();
						llCnt += std::get<1>(v);
						llMean += (std::get<0>(v) - llMean) * std::get<1>(v) / llCnt;
						steps += std::get<2>(v);
					}
					futures.clear();
				}
				return totalSteps;
			}

			template<class VocabToStrFn>
			std::ostream& printParameters(std::ostream& out, VocabToStrFn&& fn) const
			{
				out << "Total Params: " << vocabs.size() << "\n" << std::endl;
				for (size_t i = 1; i < ptrs.size(); ++i)
				{
					size_t b = ptrs[i - 1], e = ptrs[i];
					size_t segSize = e - b;
					if (segSize <= 1) continue;

					auto segment = logits.segment(b, segSize);
					float logsum = std::log((segment - segment.maxCoeff()).exp().sum());
					auto ls = (segment - (segment.maxCoeff() + logsum)).eval();
					out << fn(i - 1) << "  LM: " << ls[segSize - 1] << "\n";

					Vector<size_t> idcs(segSize - 1);
					std::iota(idcs.begin(), idcs.end(), 0);
					std::sort(idcs.begin(), idcs.end(), [&](size_t a, size_t b)
						{
							return ls[a] > ls[b];
						});

					for (size_t j : idcs)
					{
						out << fn(i - 1) << "\t" << fn(vocabs[b + j]) << " : " << ls[j] << "\n";
					}
					out << std::endl;
				}
				return out;
			}

			utils::MemoryOwner convertToModel(float trimThreshold = -15) const
			{
				Header header = { 0, };
				header.vocabSize = ptrs.size() - 1;
				header.keySize = sizeof(VocabTy);
				header.windowSize = windowSize;
				header.compressed = 0;

				size_t finalVocabSize = 0;
				Vector<float> discnts(header.vocabSize);
				Vector<std::pair<Vector<VocabTy>, Vector<float>>> compensations(header.vocabSize);

				for (size_t i = 1; i < ptrs.size(); ++i)
				{
					size_t b = ptrs[i - 1], e = ptrs[i];
					size_t segSize = e - b;

					if (segSize <= 1)
					{
						discnts[i - 1] = 0;
						continue;
					}
					auto segment = logits.segment(b, segSize);
					float logsum = std::log((segment - segment.maxCoeff()).exp().sum());
					auto ls = (segment - (segment.maxCoeff() + logsum)).eval();
					discnts[i - 1] = ls[segSize - 1];

					if ((ls.head(segSize - 1) > trimThreshold).any())
					{
						for (size_t j = 0; j < segSize - 1; ++j)
						{
							if (ls[j] < trimThreshold) continue;
							compensations[vocabs[b + j]].first.emplace_back(i - 1);
							compensations[vocabs[b + j]].second.emplace_back(ls[j]);
							++finalVocabSize;
						}
					}
				}

				size_t totalModelSize = sizeof(Header);
				totalModelSize += header.vocabSize * sizeof(VocabTy);
				totalModelSize += finalVocabSize * sizeof(VocabTy);
				totalModelSize += header.vocabSize * sizeof(float);
				totalModelSize += finalVocabSize * sizeof(float);
				totalModelSize += header.vocabSize;

				utils::MemoryOwner ret{ totalModelSize };
				auto* ptr = reinterpret_cast<char*>(ret.get());
				*reinterpret_cast<Header*>(ptr) = header;
				auto* ks = reinterpret_cast<VocabTy*>(ptr += sizeof(Header));
				for (auto& v : compensations)
				{
					*ks++ = v.first.size();
				}
				ks = reinterpret_cast<VocabTy*>(ptr += header.vocabSize * sizeof(VocabTy));
				for (auto& v : compensations)
				{
					ks = std::copy(v.first.begin(), v.first.end(), ks);
				}
				auto* fs = reinterpret_cast<float*>(ptr += finalVocabSize * sizeof(VocabTy));
				std::copy(discnts.begin(), discnts.end(), fs);
				fs = reinterpret_cast<float*>(ptr += header.vocabSize * sizeof(float));
				for (auto& v : compensations)
				{
					fs = std::copy(v.second.begin(), v.second.end(), fs);
				}
				
				auto us = reinterpret_cast<uint8_t*>(fs);
				us = std::copy(vocabValidness.begin(), vocabValidness.end(), us);

				return ret;
			}
		};
	}
}
