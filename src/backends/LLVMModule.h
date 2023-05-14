#pragma once
#include "CodeGenModule.h"
#include "CodeGenCompiler.h"
#pragma warning (disable: 4146)
#include "LLVMCompiler.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/MCJIT.h"

#include <fstream>

namespace K3 {
	namespace Backends {
		class CppHeader {
			std::stringstream declarations;
			std::stringstream classWrapper;
			std::ofstream header;
			Type outputFrame;
			std::string className;
			std::string argumentType, resultType;
			std::unordered_set<std::string> symbols;
			std::unordered_map<Type, std::string> generatedTypes;

			std::string CSymbolize(std::string input) {
				bool wasAlpha = false;
				std::string out;
				
				if (!isalpha(input.front())) {
					out = "_";
				}

				for (auto c : input) {
					if (isalnum(c)) {
						if (wasAlpha) {
							out.push_back(tolower(c));
						} else {
							out.push_back(toupper(c));
						}
						wasAlpha = true;
					} else {
						wasAlpha = false;
					}
				}
				return out;
			}

			struct ConfigValue {
				std::string type;
				std::string name;
			};

		public:
			
			void Open(std::string path, std::string prefix, const Type& input, const Type& output) {
				header.open(path);
				if (header.is_open() == false) throw std::runtime_error("Could not open '" + path + "' for writing a C/C++ header");

				className = prefix;

				header << "#pragma once\n\n#include <stdint.h>\n#include <stdlib.h>\n\n#pragma pack(push)\n#pragma pack(1)\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
				
				std::string opaqueInstance = prefix + "InstancePtr";
				
				argumentType = GenerateType(prefix + "InputType", input);
				resultType = GenerateType(prefix + "OutputType", output);

				header << "\ntypedef void* " << opaqueInstance << ";\n\n";

				header
					<< "// Returns the size of instance in bytes\n"
					<< "int64_t " << prefix << "GetSize();\n"
					<< "\n// (Internal) helper for setting global configuration\n"
					<< "void " << prefix << "SetConfigurationSlot(int32_t slotIndex, const void* data);\n"
					<< "\n// Reset the state of an instance, which is a pointer to `GetSize()` allocated bytes\n"
					<< "void " << prefix << "Initialize(" << opaqueInstance << ", " << argumentType << "*);\n"
					<< "\n// (Internal) helper for accessing the external input binding pointers of this instance\n"
					<< "void** " << prefix << "GetValue(" << opaqueInstance << ", int32_t slotIndex);\n"
					<< "\n";
			}

			void Close() {
				if (header.is_open()) {

					auto wrapperName = className;
					if (wrapperName.empty()) {
						wrapperName = "Dsp";
					}

					header << "\n// Convenience constructor for allocating memory, setting configuration and initializing an instance\nstatic " << className << "InstancePtr " << className << "Construct(";
					bool first = true;
					for (auto& cv : configSlots) {

						if (first) {
							first = false;
						} else {
							header << ", ";
						}

						header << "const " << cv.type << "* " << cv.name;
					}
					
					header
						<< ") {\n"
						<< "\tvoid* memory = calloc(1, " << className << "GetSize());\n";

					for (auto& cv : configSlots) {
						header << "\t" << className << "Configure" << cv.name << "(" << cv.name << ");\n";
					}
					header
						<< "\t" << className << "Initialize(memory, NULL);\n"
						<< "\treturn memory;\n}\n\n";


					header << "#ifdef __cplusplus\n}\n\n";

					// c++ convenience wrapper
					header 
						<< "// c++ wrapper\n"
						<< "class " << wrapperName << " {\n\t" << className << "InstancePtr instance = nullptr;\npublic:\n\t"
						<< wrapperName << "(";
						
					first = true;
					for (auto& cv : configSlots) {
						if (first) {
							first = false;
						} else {
							header << ", ";
						}

						header << "const " << cv.type << "* " << cv.name;
					}

					header << ") { instance = " << className << "Construct(";

					first = true;
					for (auto& cv : configSlots) {
						if (first) {
							first = false;
						} else {
							header << ", ";
						}

						header << cv.name;
					}

					header << "); }\n\t~" << wrapperName << "() { if (instance) free(instance); }\n"
						//<< "#if __cplusplus > 199711L\n"
						<< "\t"
						<< wrapperName << "(" << wrapperName << " const&) = delete;\n\t"
						<< wrapperName << "(" << wrapperName << "&& from) { instance = from.instance; from.instance = nullptr; };\n\t"
						<< "void operator=(" << wrapperName << " const&) = delete;\n\t"
						<< wrapperName << "& operator=(" << wrapperName << "&& from) { auto tmp = from.instance; from.instance = instance; instance = tmp; return *this; }\n"
						// << "#endif\n"
						"\n";

					header 
						<< classWrapper.rdbuf() << "};\n";
					
					header << "\n#endif\n#pragma pack(pop)";
					header.close();
				}
			}

			void GenerateMembers(const Type& type, std::vector<std::string>& list) {
				if (type.GetSize() < 1) return;
				else if (type.IsFloat32()) list.emplace_back("float");
				else if (type.IsFloat64()) list.emplace_back("double");
				else if (type.IsInt32()) list.emplace_back("int32_t");
				else if (type.IsInt64()) list.emplace_back("int64_t");
				else if (type.IsPair()) {
                    Type current = type;
                    while(current.IsPair()) {
                        GenerateMembers(current.First(), list);
                        current = current.Rest();
                    }
                    GenerateMembers(current, list);
                    
				} else if (type.IsUserType()) {
					GenerateMembers(type.UnwrapUserType(), list);
				} else if (type.IsNativeVector()) {
					for (int i = 0; i < type.GetVectorWidth(); ++i) {
						GenerateMembers(type.GetVectorElement(), list);
					}
				} else {
					std::stringstream typeName;
					type.OutputText(typeName);
					list.emplace_back("kronos_" + typeName.str());
				}
			}

			std::string GenerateType(std::string baseName, const Type& type) {

				std::vector<std::string> members;
				GenerateMembers(type, members);

				switch (members.size()) {
					case 0: return "void";
					case 1: return members.front();
					default:
					{
						bool heterogeneous = false;
						for (int i = 1; i < members.size(); ++i) {
							if (members[i] != members[0]) {
								heterogeneous = true;
								break;
							}
						}

						if (!heterogeneous) {
							header << "typedef " << members.front() << " " << baseName << "[" << members.size() << "];\n";
						} else {
							header << "typedef struct {\n";
							for (int i = 0; i < members.size();) {
								int b = i;
								for (i = b + 1; i < members.size() && members[i] == members[b]; ++i);
								int dim = i - b;
								if (dim > 1) {
									header << "\t" << members[b] << " v" << b << "[" << dim << "];\n";
								} else {
									header << "\t" << members[b] << " v" << b << ";\n";
								}
							}

							header << "} " << baseName << ";\n";
						}
						return baseName;
					}
				}
			}

			void DeclareDriver(std::string linkerSymbol) {
				header << "void " << className << linkerSymbol << "(" << className << "InstancePtr, " << resultType << "* outputBuffer, int32_t numFrames);\n";
				classWrapper << "\tvoid " << linkerSymbol << "(" << resultType << "* outputBuffer, int32_t numFrames = 1) { ::"
					<< className << linkerSymbol << "(instance, outputBuffer, numFrames); }\n";
			}

			std::vector<ConfigValue> configSlots;

			void DeclareSlot(std::string slotName, int slotIndex, const Type& inputType, bool constructorParameter, std::string streamingDriver) {
				if (header.is_open()) {
					auto typeName = GenerateType(className + slotName + "InputType", inputType);
					auto cName = CSymbolize(slotName);

					if (constructorParameter) {
						header << "\n// You must set this before calling `" << className << "Initialize()`\n";
						configSlots.push_back(ConfigValue{ typeName, cName });
						header << "static void " << className << "Configure" << cName << "(const " << typeName << "* data) { \n\t"
							<< className << "SetConfigurationSlot(" << slotIndex << ", data);\n}\n";
					} else {
						if (streamingDriver.size()) {
							header << "// Provide an array of N values every time before calling " << className << streamingDriver << " with `numFrames = N`\n";
							header << "// The driver will move the pointer ahead for every processed frame.\n";
						}

						header << "static void " << className << "Set" << cName << "(" << className << "InstancePtr instance, const " << typeName << "* " << cName << ") {\n"
							<< "\t*" << className << "GetValue(instance, " << slotIndex << ") = (void*)" << cName << ";\n}\n\n";

						classWrapper
							<< "\tvoid Set" << cName << "(const " << typeName << "* inputBuffer) { ::"
							<< className << "Set" << cName << "(instance, inputBuffer); }\n";
					}
				}
			}
		};

		class LLVM : public CodeGenModule {
			llvm::LLVMContext& Context; // must be before Module M
			
			LLVM(const LLVM&);
			LLVM& operator=(const LLVM&) = delete;
			std::unordered_map<Reactive::DriverSet, llvm::Function*, dshash> activations;

			llvm::Function* GetPartialSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indiceSet, int longCounterTreshold);
			llvm::Function* GetSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indiceSet, int longCounterTreshold);
			llvm::Function * CombineSubActivations(const std::string & name, const std::vector<llvm::Function*>& superClockFrames);
			llvm::Function* GetActivation(const std::string& nameTemplate, CTRef graph, const Type& signature, llvm::Function *sizeOfStateStub, llvm::Function *sizeOfStub);
			int firstCounterBitMaskIndex = 0;
		protected:
			CppHeader cppHeader;
			std::unordered_map<Type, llvm::Function*> inputCall;
			std::unique_ptr<llvm::Module> M;
			void MakeIR(Kronos::BuildFlags);
			void Optimize(int level, std::string mcpu, std::string march, std::string mfeat);
		public:
			LLVM(CTRef AST, const Type& argType, const Type& resType);
			~LLVM();

			llvm::LLVMContext& GetContext();
			std::unique_ptr<llvm::Module>& GetModule() { return M; }
			void Build(Kronos::BuildFlags flags);
			krt_class* JIT(Kronos::BuildFlags flags);
			virtual void AoT(const char *prefix, const char *fileType, std::ostream& writeToStream, Kronos::BuildFlags flags, const char* triple, const char *mcpu, const char *march, const char *mfeat);
		};
	};
}
