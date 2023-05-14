#pragma once

#include "CodeGenModule.h"
#include <sstream>

namespace K3 {
	namespace Backends {
		template <typename TCodeGen>
		class GenericCodeGen : public TCodeGen {
		public:
			using ModuleTy = typename TCodeGen::ModuleTy;
			using FunctionTy = typename TCodeGen::FunctionTy;
			using FunctionTyTy = typename TCodeGen::FunctionTyTy;
			using TypeTy = typename TCodeGen::TypeTy;
			using ValueTy = typename TCodeGen::ValueTy;
			using VariableTy = typename TCodeGen::VariableTy;
			using BuilderTy = typename TCodeGen::BuilderTy;
			using BlockTy = typename TCodeGen::BlockTy;
			using SizeDictionaryTy = std::unordered_map<const Nodes::Subroutine*, FunctionTy>;

			struct dshash {
				size_t operator()(const Reactive::DriverSet& ds) const {
					                                    // drivers may be stored in different order for small sets.
					                                    // thus use a crappy xor hash combinator to ensure order
						                                // is transparent to hash.
					size_t hash(0);
					ds.for_each([&](const Type& t) { hash ^= t.GetHash(); });
					return hash;
				}
			};		

			using TCodeGen::GetIndex;
			using TCodeGen::GetArgumentIndex;
			using TCodeGen::GetArgumentType;
			using TCodeGen::GetResultType;
			using TCodeGen::Int32Ty;
			using TCodeGen::VoidTy;
			using TCodeGen::PtrTy;
			using TCodeGen::CreateFunction;
			using TCodeGen::GetFunctionTy;
			using TCodeGen::GetActivationMatrix;
			using TCodeGen::ComputeAuspiciousVectorLength;
			using TCodeGen::CombineRows;
			using TCodeGen::GetCounterSet;
			using TCodeGen::RegisterSignalMaskSlot;
			using TCodeGen::AST;

			GenericCodeGen(ModuleTy M, CTRef AST, const Type& arg, const Type& res) :TCodeGen(M, AST, arg, res) { }
		protected:
			using TCodeGen::globalKeyTable;
			std::unordered_map<Reactive::DriverSet, FunctionTy, dshash> activations;
			SizeDictionaryTy sizeOfFuncs;

			FunctionTy GetPartialSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indices, int longCounterTreshold) {
				FunctionTy result;

				CounterIndiceSet longCounters;
				if (longCounterTreshold) {
					for (auto &c : indices) {
						if (c.second.GetDivider() >= longCounterTreshold) longCounters.insert(c);
					}
				}

				if (longCounters.empty()) {
					// build activation state with this set of drivers and counter indices
//					CompilationPass pass(*this, sizeOfFuncs, indices, name, graph, Backends::BuilderPass::Reactive);
//					drivers.for_each([&pass](const Type& d) { pass.insert(d); });
					result = TCodeGen::CompilePass(name, Backends::BuilderPass::Reactive, drivers, indices);
				} else {
					CounterIndiceSet shortCounters;
					Reactive::DriverSet shortDrivers;
					drivers.for_each([&](const Type &d) {
						auto f = indices.find(d);
						if (f == indices.end() || f->second.GetDivider() < longCounterTreshold) {
							shortDrivers.insert(d);
							if (f != indices.end()) {
								shortCounters.insert(*f);
							}
						}
					});

					auto shortCounterActivation = GetPartialSubActivation("Common_" + name, graph, shortDrivers, shortCounters, longCounterTreshold);

					// if there is only one long counter index, its state is known statically due to the general long counter test. 
					// in that case, generate partial activation with that index mask omitted (always active)
					auto longCounterActivation = GetPartialSubActivation("Rare_" + name, graph, drivers, longCounters.size() > 1 ? indices : shortCounters, 0);

					auto longCounterTest = CreateFunction("CheckLong_" + name, GetFunctionTy(shortCounterActivation), false);
					longCounterTest.FastCConv();
					longCounterTest.NoThrow();

					BuilderTy b{ longCounterTest };
					ValueTy needsLongCounters = b.False();
					for (auto &c : longCounters) {
						auto counter = b.PtrToInt(b.GetSlot(c.second.GetIndex()));
						needsLongCounters = b.OrInt32(needsLongCounters, b.LeSInt32(counter, b.Const(0)));
					}

					std::vector<ValueTy> passArgs;
					for (int i = 0;i < b.NumFnArgs();++i) {
						passArgs.emplace_back(b.FnArg(i));
					}

					b.If(needsLongCounters, [&](BuilderTy& then_) {
						then_.Ret(then_.Call(longCounterActivation, passArgs, true));
					}, [&](BuilderTy& else_) {
						else_.Ret(else_.Call(shortCounterActivation, passArgs, true));
					});

					result = longCounterTest;
				}
				result.Complete();
				return result;
			}

			static void DoClockCounters(BuilderTy& stub, const CounterIndiceSet& indices, const Reactive::DriverSet& ds) {
				std::unordered_map<int, ValueTy> counterBits;

				for (auto idx : indices) {
					if (idx.second.GetDivider() > 1 && ds.find(idx.first)) {
						auto counterPtr = stub.GetSlot(idx.second.GetIndex()); // stub.CreateConstGEP1_32(selfPtr, idx.second.GetIndex());
						int divider = (int)idx.second.GetDivider();

						auto CounterVar = stub.TmpVar(stub.PtrToInt(counterPtr));  // stub.CreatePtrToInt(stub.CreateLoad(counterPtr, false), stub.getInt32Ty());
						auto CounterIsZero = stub.TmpVar(stub.EqInt32(CounterVar, stub.Const(0))); // CreateICmpEQ(Counter, stub.getInt32(0));
						auto Counter = stub.Select(CounterIsZero, stub.Const(divider), CounterVar);
						Counter = stub.SubInt32(Counter, stub.Const(1));

						stub.SetSlot(idx.second.GetIndex(), stub.IntToPtr(Counter));

						assert(idx.second.BitMaskIndex() >= 0 && "Bit mask not assigned to counter");
						int wordIdx = (int)idx.second.BitMaskIndex() / 32;
						int subIdx = (int)idx.second.BitMaskIndex() % 32;
						int outSubIdx(subIdx);

						auto cf = counterBits.find(wordIdx);
						if (cf == counterBits.end()) {
							cf = counterBits.emplace(wordIdx, stub.Const(0)).first;
						}

						auto CounterBit = stub.Select(CounterIsZero, stub.Const(1u << outSubIdx), stub.Const(0));

						counterBits[wordIdx] = stub.OrInt32(counterBits[wordIdx], CounterBit);
						//                auto self = stub.CreateBitCast(selfPtr, stub.getInt32Ty()->getPointerTo());
/*						auto SetBit = stub.XorInt32(
							stub.Get(counterBits[wordIdx]),
								stub.AndInt32(
									stub.XorInt32(
										WideTruth,
										stub.Get(counterBits[wordIdx])), SwitchMask));
						stub.Set(counterBits[wordIdx], SetBit);*/
					}
				}

				for (auto cb : counterBits) {
					stub.StoreSignalMaskWord(cb.first * 32, cb.second);
				}
			}

			FunctionTy CombineSubActivations(const std::string& name, const std::vector<FunctionTy>& superClockFrames) {
				if (superClockFrames.size() == 1) return superClockFrames[0];
				auto superTick = CreateFunction(name + "_super", GetFunctionTy(superClockFrames[0]), false);
				BuilderTy stub{ superTick };

				for (int f = 0;;++f) {
					std::vector<ValueTy> params;
					for (int i = 0; i < stub.NumFnArgs(); ++i) {
						params.emplace_back(stub.FnArg(i));
					}
					if (f == superClockFrames.size() - 1) {
						stub.Ret(stub.PureCall(superClockFrames[f], params, false));
						superTick.Complete();
						return superTick;
					} else {
						stub.Call(superClockFrames[f], params, false);
					}
				}
				KRONOS_UNREACHABLE;
			}

			FunctionTy GetSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indices, int longCounterTreshold) {
				auto f(activations.find(drivers));
				if (f != activations.end()) {
					return f->second;
				}

				// get the code to activate graph
				auto activationState = GetPartialSubActivation(name, graph, drivers, indices, longCounterTreshold);

				// add stream input iterators				
				auto result = CreateFunction(name + "_schd", GetFunctionTy(activationState), false);
				result.NoInline();

				BuilderTy stub{ result };
					
				// process clock counters
				DoClockCounters(stub, indices, drivers);

				std::vector<ValueTy> passArgs;
				for (int i = 0;i < stub.NumFnArgs();++i) {
					passArgs.emplace_back(stub.FnArg(i));
				}

				auto retval = stub.TmpVar(stub.PureCall(activationState, passArgs, true));
				//retval->setDoesNotThrow();

				for (auto& vk : globalKeyTable) {
					drivers.for_each([&](const Type& d) {
						DriverSignature dsig = d;
						if (dsig.GetMetadata() == vk.second.clock &&
							dsig.GetMul() == vk.second.relativeRate.first &&
							dsig.GetDiv() == vk.second.relativeRate.second) {

							auto slotIndex = GetIndex(vk.second.uid);

							// if there's no input associated with the clock, skip
							if (vk.second.uid != nullptr) {

								auto f = indices.find(d);
								if (f != indices.end()) {
									int subidx(-1);
									auto mask = stub.GetSignalMaskWord((int)f->second.BitMaskIndex(), subidx);
									auto test = stub.NeInt32(stub.AndInt32(mask, stub.Const(1 << subidx)), stub.Const(0));

									stub.If(test, [=](BuilderTy& then_) {
										then_.SetSlot(slotIndex,
													  then_.AddInt32(then_.PtrToInt(then_.GetSlot(slotIndex)),
																then_.Const((int)vk.second.data.GetSize())));
									});
								} else {
									stub.SetSlot(slotIndex,
												  stub.AddInt32(stub.PtrToInt(stub.GetSlot(slotIndex)),
															stub.Const((int)vk.second.data.GetSize())));
								}
							}
						}
					});
				}

				stub.Ret(retval);

				activations.insert(std::make_pair(drivers, result));
				return result;
			}

			std::vector<FunctionTy> GetActivationVector(const std::string& nameTemplate, const Type& signature) {
				int jitter(0), tmp(0);

				ActivationMatrix Activation = GetActivationMatrix(signature, 1, jitter);
				const int vectorIterationSize = ComputeAuspiciousVectorLength(Activation, 16);
				if (vectorIterationSize > 1) {
					auto amtx = GetActivationMatrix(signature, vectorIterationSize, tmp);
					Activation = CombineRows(amtx, jitter);
				}

				CounterIndiceSet Indices = GetCounterSet(Activation, vectorIterationSize);

				// assign bitmasks to counters
				int bitMaskIdx = firstCounterBitMaskIndex;
				for (auto &i : Indices) {
					RegisterSignalMaskSlot(bitMaskIdx);
					i.second.BitMaskIndex() = bitMaskIdx++;
				}

				std::vector<FunctionTy> subFrameFunctions(vectorIterationSize);

				int ticksPerFrame = (int)(Activation.size() / vectorIterationSize);

				for (size_t i = 0; i < subFrameFunctions.size(); ++i) {
					std::stringstream frameName;
					frameName << nameTemplate << i;

					// make a subframe
					std::vector<FunctionTy> superClockFrames;
					for (int j = 0; j < ticksPerFrame; ++j) {
						Reactive::DriverSet activationState;
						for (auto& m : Activation[i*ticksPerFrame + j]) {
							DriverSignature sig(m.GetDriver());
							activationState.insert(m.GetDriver());
						}
						superClockFrames.emplace_back(
							GetSubActivation(frameName.str() + (j ? "_oversmp" + std::to_string(j) : ""s), 
											 AST, activationState, Indices, (int)Activation.size() * 4));
					}

					subFrameFunctions[i] = CombineSubActivations(nameTemplate, superClockFrames);
				}

				return subFrameFunctions;
			}

			FunctionTy GetActivation(const std::string& nameTemplate, CTRef graph, const Type& signature) {
				auto subFrameFunctions = GetActivationVector(nameTemplate, signature);
				auto vectorIterationSize = (int)subFrameFunctions.size();
				FunctionTy vectorDriver, scalarDriver, driverStub;
				std::vector<TypeTy> paramTys{ PtrTy(), PtrTy(), Int32Ty() };
				std::vector<TypeTy> scalarParamTys{ PtrTy(), PtrTy(), Int32Ty(), Int32Ty() };

				vectorDriver = CreateFunction(nameTemplate + "_vector", VoidTy(), paramTys, false);
				vectorDriver.NoThrow();
				vectorDriver.FastCConv();
				{
					BuilderTy b{ vectorDriver };

					auto instance = b.FnArg(0, PtrTy());
					auto output = b.FnArg(1, PtrTy());
					auto loopCount = b.FnArg(2, Int32Ty());

					ValueTy argPtr;

					if (GetArgumentType().GetSize()) {
						argPtr = b.GetSlot(GetArgumentIndex());
					} else {
						argPtr = b.UndefConst(PtrTy());
					}

					auto outStride = vectorIterationSize * (int)GetResultType().GetSize();
					auto instVar = b.TmpVar(instance);
					auto argVar = b.TmpVar(argPtr);
					auto outVar = b.TmpVar(output);

					b.Loop(loopCount, [=](const std::string& /*breakLabel*/, BuilderTy& lb, ValueTy counter) {
						// provide output as input if mix bus is requested. 
						// this violates some of the noalias we claim, have to see
						// if it miscompiles stuff
						auto mixBusKey = globalKeyTable.find(Type::Pair(Type("unsafe"), Type("accumulator")));
						auto outBase = lb.TmpVar(lb.Offset(outVar, lb.MulInt32(counter, lb.Const(outStride))));

						for (size_t i(0); i < subFrameFunctions.size(); ++i) {
							if (mixBusKey != globalKeyTable.end()) {
								lb.SetSlot(GetIndex(mixBusKey->second.uid),
										   lb.Offset(outBase, lb.Const(int(i * GetResultType().GetSize()))));
							}
							lb.Call( subFrameFunctions[i], { instVar, argVar, 
									lb.Offset(outBase, lb.Const(int(i * GetResultType().GetSize()))) }, true);
						}
					});

					b.Ret();
					vectorDriver.Complete();
				}

				if (vectorIterationSize > 1) {
					scalarDriver = CreateFunction(nameTemplate + "_remainder", VoidTy(), scalarParamTys, false);
					{
						scalarDriver.FastCConv();
						scalarDriver.NoThrow();

						BuilderTy b{ scalarDriver };

						auto instance = b.TmpVar(b.FnArg(0, PtrTy()));
						auto output = b.TmpVar(b.FnArg(1, PtrTy()));
						auto loopCount = b.TmpVar(b.FnArg(2, Int32Ty()));
						auto initSubPhase = b.FnArg(3, Int32Ty());

						decltype(instance) argPtr( 
							scalarDriver,
							GetArgumentType().GetSize() != 0
							? b.GetSlot(GetArgumentIndex())
							: b.UndefConst(PtrTy())
						);

						auto mixBusKey = globalKeyTable.find(Type::Pair(Type("unsafe"), Type("accumulator")));

						auto subpVar = b.LVar("subphase", Int32Ty());
						auto counter = b.LVar("remainderCount", Int32Ty());
						auto outFrame = b.LVar("outFrame", Int32Ty());
						b.Set(counter, b.Const(0));
						b.Set(subpVar, initSubPhase);
						b.Set(outFrame, output);

						b.Loop([&](const std::string& /*break*/, BuilderTy& lb) mutable {
							std::vector<BlockTy> subFrames;
							for (int i = 0;i < subFrameFunctions.size();++i) {
								subFrames.emplace_back("subframe" + std::to_string(i));
								BuilderTy frame{ scalarDriver, subFrames.back() };
								auto ldCounter = frame.TmpVar(frame.AddInt32(frame.Get(counter, Int32Ty()), frame.Const(1)));
								frame.If(
									frame.GtSInt32(ldCounter, loopCount),
									[=](BuilderTy& done) {
										done.Ret();
								});
								auto frameOut = frame.TmpVar(frame.Get(outFrame, Int32Ty()));
								frame.Set(outFrame, frame.Offset(frameOut, frame.Const((int)GetResultType().GetSize())));

								if (mixBusKey != globalKeyTable.end()) {
									frame.SetSlot(GetIndex(mixBusKey->second.uid), frameOut);
								}

								frame.Call(subFrameFunctions[i], { instance, argPtr, frameOut }, true);
								frame.Set(counter, ldCounter);
							}

							lb.Switch(lb.Get(subpVar, Int32Ty()), subFrames);
							lb.Set(subpVar, lb.Const(0));
						});

						b.Ret();
						scalarDriver.Complete();
					}
				}


				driverStub = CreateFunction(nameTemplate, VoidTy(), paramTys, true);
				{
					BuilderTy b{ driverStub };

					auto instance = b.TmpVar(b.FnArg(0, PtrTy()));
					auto output = b.TmpVar(b.FnArg(1, PtrTy()));
					auto loopCount = b.TmpVar(b.FnArg(2, Int32Ty()));
					auto counter = b.LVar("loopCount", Int32Ty());
					auto outPtr = b.LVar("out", PtrTy());
					b.Set(counter, loopCount);
					b.Set(outPtr, output);

					if (vectorIterationSize > 1) {
						// allocate ivar to track subphase within vectored process
						unsigned vectorSubphaseOffset = GetIndex();
						// may need prealign and remainder
						auto sub = b.TmpVar(b.RemUInt32(b.PtrToInt(b.GetSlot(vectorSubphaseOffset)), b.Const(vectorIterationSize)));
						b.SetSlot(vectorSubphaseOffset, b.IntToPtr(b.AddInt32(sub, loopCount)));

						auto prealign = 
							b.TmpVar(b.Select(
									b.NeInt32(sub, b.Const(0)),
										b.SubInt32(b.Const(vectorIterationSize), sub),
										b.Const(0)));

						auto prealignCount = b.TmpVar(b.Select(b.LtSInt32(prealign, loopCount), prealign, loopCount));

						b.If( b.NeInt32(prealignCount, b.Const(0)), 
							 [=](BuilderTy& then) {
								then.Call(scalarDriver, { instance, output, prealignCount, sub }, true);
						});

						b.Set(counter, b.SubInt32(loopCount, prealignCount));
						b.Set(outPtr, 
							  b.Offset(b.Get(outPtr, PtrTy()), 
									   b.MulInt32(prealignCount, b.Const((int)GetResultType().GetSize()))));

//						remainder_count = b.CreateSub(loopCount, b.CreateAdd(prealign_count, b.CreateMul(vector_count, b.getInt32(vectorIterationSize))));
//						remainder_out = b.CreateGEP(output, b.CreateMul(b.CreateSub(loopCount, remainder_count), b.getInt32(GetResultType().GetSize())));

					} 

					auto vectorCount = b.TmpVar(b.DivSInt32(b.Get(counter, Int32Ty()), b.Const(vectorIterationSize)));
					b.Call( vectorDriver, { instance, b.Get(outPtr, PtrTy()), vectorCount }, true);

					if (vectorIterationSize > 1) {
						auto vectoredFrames = b.TmpVar(b.MulInt32(vectorCount, b.Const(vectorIterationSize)));
						auto remainderCount = b.TmpVar(b.SubInt32(b.Get(counter, Int32Ty()), vectoredFrames));
						auto remainderOut = b.Offset(b.Get(outPtr, PtrTy()), b.MulInt32(vectoredFrames, b.Const((int)GetResultType().GetSize())));

						b.If(b.NeInt32(remainderCount, b.Const(0)), [=](BuilderTy& then) {
							then.Call(scalarDriver, { instance, remainderOut, remainderCount, then.Const(0) }, true);
						});
					}

					b.Ret();
					driverStub.Complete();
				}

				return driverStub;
			}

			void BuildActivationState(const string& nameTemplate, BuilderTy& stub, ActivationMatrix& mtx, ValueTy indvar, ValueTy outputInit, ValueTy inputInit, ValueTy inputPtrPtr, int jitter);

			int firstCounterBitMaskIndex = 0;
			std::unordered_map<Type, FunctionTy> inputCall;
			ModuleTy M;
		public:

			ModuleTy GetModule() { return M; }

/*			static void StoreSignalMaskWord(BuilderTy&, int bitIdx, ValueTy word);
			static ValueTy GetSignalMaskWord(BuilderTy&, int bitIdx, int& outSubIdx);*/
		};
	}
}