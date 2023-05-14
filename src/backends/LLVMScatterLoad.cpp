#pragma warning(disable:4146)
#include "LLVMCompiler.h"
#include "Native.h"
#include "NativeVector.h"
#include <set>
#include <deque>

#undef max

namespace K3 {
	using namespace Backends;
	using namespace std;
	namespace Nodes{

		static const int extractOffsetBase = -65536;

		static tuple<CTRef,int64_t> GetBaseOffset(CTRef ptr, int64_t offset)
		{
			const ExtractVectorElement *extr;
			if (ptr->Cast(extr) && offset == -1)
			{
				return make_tuple(extr->GetUp(0),extractOffsetBase + (extr->GetIndex() * extr->FixedResult().GetSize()));
			}

			if (offset == -1)
			{
				Dereference *dr;
				if (ptr->Cast(dr)) return GetBaseOffset(dr->GetUp(0),0);
				else return make_tuple(ptr,-1);
			}
			else
			{
				const Offset *off;
				const Native::Constant *c;
				if (ptr->Cast(off) && off->GetUp(1)->Cast(c))
				{
					return GetBaseOffset(off->GetUp(0),offset + c->AsInteger());
				}
				else return make_tuple(ptr,offset);
			}
		}

		static llvm::Value* PadVector(llvm::IRBuilder<>& ir, llvm::Value* vec, unsigned toSize)
		{
			if (vec->getType()->getVectorNumElements() < toSize)
			{
				vector<llvm::Constant*> indices(toSize);
				for(unsigned i(0);i<indices.size();++i)
				{
					if (i<vec->getType()->getVectorNumElements()) indices[i] = ir.getInt32(i);
					else indices[i] = UndefValue::get(ir.getInt32Ty());
				}
				return ir.CreateShuffleVector(vec,UndefValue::get(vec->getType()),ConstantVector::get(indices));
			}
			else return vec;
		}

		LLVMValue PackVector::Compile(LLVMTransform& lt, ActivityMaskVector* active) const
		{
			/* gather load pointers into base + offset pairs */
			size_t elementSz(FixedResult().GetVectorElement().GetSize());
			multimap<tuple<CTRef,int64_t>,int64_t> loadPointers;
			for(unsigned i(0);i<GetNumCons();++i)
			{
				loadPointers.insert(make_pair(GetBaseOffset(GetUp(i),-1),i));
			}

			/* combine adjacent load pointers */
	 		/* dictoionary: [base,offset] -> [load slot x target slot] */
			map<tuple<CTRef,int64_t>,deque<vector<int64_t>>> combinedLoads;
			
			for(auto l(loadPointers.rbegin());l!=loadPointers.rend();++l)
			{
				CTRef base;
				int64_t offset;
				tie(base,offset) = l->first;

				auto f(combinedLoads.find(make_tuple(base,offset + elementSz)));
				if (f==combinedLoads.end() || offset == -1)
				{
					/* extend by skipping one? */
					auto sf(combinedLoads.find(make_tuple(base, offset + 2 * elementSz)));
					if (sf != combinedLoads.end() && offset >= 0)
					{
						/* make new region by extending down */
						combinedLoads[l->first] = sf->second;
						combinedLoads[l->first].push_front(vector<int64_t>());
						combinedLoads[l->first].push_front(vector<int64_t>(1,l->second));
						combinedLoads.erase(sf);
					}
					else
					{
						/* do we have multiple targets for this load? */
						auto f2(combinedLoads.find(l->first));
						if (f2 == combinedLoads.end())
						{
							/* first load to this region */
							combinedLoads[l->first].push_back(vector<int64_t>(1,l->second));
						}
						else
						{
							/* add target slot to matrix */
							f2->second[0].push_back(l->second);
						}
					}
				}
				else
				{
					/* make new region by extending down */
					combinedLoads[l->first] = f->second;
					combinedLoads[l->first].push_front(vector<int64_t>(1,l->second));
					combinedLoads.erase(f);
				}
			}


			/* load each of the vectors */
			llvm::Value* vec = llvm::UndefValue::get(lt.GetType(FixedResult()));
			vector<llvm::Constant*> shufflemask(vec->getType()->getVectorNumElements());
			for(auto load : combinedLoads)
			{
				llvm::Value *insertion;
				CTRef base;
				int64_t offset;
				tie(base,offset) = load.first;

				if (offset==-1) // not vectored source
				{
					insertion = llvm::UndefValue::get(vec->getType());
					insertion = lt.GetBuilder()
								  .CreateInsertElement(insertion,lt(base),lt.GetBuilder().getInt32(0));					
				}
				else
				{
					auto &b(lt.GetBuilder());
					if (offset>=0)
					{
						/* align loads to even vector size to facilitate combination of odd/even scatters */
						if (load.second.size() > 1 && (load.second.size() % 2))
						{
							if (offset % (2 * elementSz) == elementSz)
							{
								load.second.push_front(vector<int64_t>());
								offset -= elementSz;
							}
							else load.second.push_back(vector<int64_t>());
						}

						/* load all elements in this vector range */
						insertion = b.CreateLoad(
							b.CreateBitCast(
								b.CreateConstGEP1_32(lt(base), offset),
								llvm::VectorType::get(vec->getType()->getVectorElementType(), load.second.size())->getPointerTo()));
					}
					else		
					{
						/* load from ExtractVector source and align to vector root by pushing null loads at the start */
						insertion = lt(base);
						offset-=extractOffsetBase;
						while(offset>0)
						{
							offset-=elementSz;
							load.second.push_front(vector<int64_t>());
						}
					}

					unsigned toSize(max(insertion->getType()->getVectorNumElements(),vec->getType()->getVectorNumElements()));
					insertion = PadVector(b,insertion,toSize);
					vec = PadVector(b,vec,toSize);
				}

				for(unsigned i(0);i<shufflemask.size();++i)
				{
					shufflemask[i] = lt.GetBuilder().getInt32(i);
					for(unsigned j(0);j<load.second.size();++j)
					{
						if (find(load.second[j].begin(),load.second[j].end(),i)!=load.second[j].end())
						{
							shufflemask[i] = lt.GetBuilder().getInt32(j + vec->getType()->getVectorNumElements());
						}
					}
				}

				vec = lt.GetBuilder().CreateShuffleVector(vec,insertion,ConstantVector::get(shufflemask));
			}
			
			// naive
			//for(unsigned i(0);i<vecType.GetVectorWidth();++i) 
			//{
			//	vec = lt.GetBuilder().CreateInsertElement(vec,*lt(GetUp(i)),lt.GetBuilder().getInt32(i));
			//}

			return vec;
		}
	};
};