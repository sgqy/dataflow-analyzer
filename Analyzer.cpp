/*
 * Analyzer.cpp
 *
 *  Created on: May 22, 2017
 *      Author: aya
 */

#include <llvm/IR/Instruction.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Analyzer.hpp"
#include "llvm_util.hpp"

#define DBG_DUMP

#ifdef DBG_DUMP
#define DBG_DUMP_FUNCN
#define DBG_DUMP_INST
#define DBG_DUMP_VAR
#define DBG_DUMP_EDGE
#define DBG_DUMP_BB
#endif

#define INDEPENDENT_BLOCK

namespace dataflow
{
    void Analyzer::Analyze(llvm::Function& F)
    {
#ifdef DBG_DUMP_FUNCN
        llvm::errs() << "$$$ begin function " << LLVMUtil::DemangleName(F.getName().str()) << " $$$\n";
#endif

        // set type of basic blocks
        for (auto BB = F.begin(); BB != F.end(); ++BB)
        {
            llvm::TerminatorInst* TI = BB->getTerminator();
            for(unsigned int i = 0; i < TI->getNumSuccessors(); ++i)
            {
                llvm::BasicBlock* succ = TI->getSuccessor(i);
                llvm::Value* inst = &*(succ->begin());
                onBranch(inst, (i == 0 ? _cond_type::Common : _cond_type::Inhibit));
            }
        }

        // make graph
        for (auto BB = F.begin(); BB != F.end(); ++BB)
        {
            for (auto II = BB->begin(); II != BB->end(); ++II)
            {
                INDEPENDENT_BLOCK
                {
                    llvm::AllocaInst* i = llvm::dyn_cast<llvm::AllocaInst>(II);
                    if(i != 0)
                    {
                        onAlloca(i);
                        continue;
                    }
                }

                INDEPENDENT_BLOCK
                {
                    llvm::LoadInst* i = llvm::dyn_cast<llvm::LoadInst>(II);
                    if(i != 0)
                    {
                        onLoad(&*i, i->getPointerOperand());
                        continue;
                    }
                }

                INDEPENDENT_BLOCK
                {
                    llvm::StoreInst* i = llvm::dyn_cast<llvm::StoreInst>(II);
                    if(i != 0)
                    {
                        onStore(i->getPointerOperand(), i->getValueOperand());
                        continue;
                    }
                }

                INDEPENDENT_BLOCK
                {
                    llvm::BinaryOperator* i = llvm::dyn_cast<llvm::BinaryOperator>(II);
                    if(i != 0)
                    {
                        onBinary(&*(BB->begin()), &*i, i->getOperand(0), i->getOperand(1));
                        continue;
                    }
                }

                INDEPENDENT_BLOCK
                {
                    llvm::CallInst* i = llvm::dyn_cast<llvm::CallInst>(II);
                    if(i != 0)
                    {
                        std::vector<llvm::Value*> args;
                        llvm::Function* f = i->getCalledFunction();
                        for(auto a = f->arg_begin(); a != f->arg_end(); ++a)
                        {
                            args.push_back(&*a);
                        }
                        onCall(&*(BB->begin()), i, args);
                        continue;
                    }
                }
            }
        }

#ifdef DBG_DUMP_VAR
        for(auto&& v : vars)
        {
            llvm::errs() << "# var # " << v.first << ": ";
            for(auto&& a : v.second)
            {
                llvm::errs() << a.first << " " << a.second << " | ";
            }
            llvm::errs() << "\n";
        }
#endif

#ifdef DBG_DUMP_EDGE
        for(auto&& e : edges)
        {
            llvm::errs() << "# edge # "
                    << e.first.first
                    << " -> "
                    << e.first.second
                    << " (" << e.second.first << "<" << e.second.second << ">)"
                    << "\n";
        }
#endif

#ifdef DBG_DUMP_BB
        for(auto&& b : blocks)
        {
            llvm::errs() << "# block # " << b.second << ": " << LLVMUtil::GetVarDescript(b.first) << "\n";
        }
#endif

#ifdef DBG_DUMP_FUNCN
        llvm::errs() << "$$$ end of function $$$\n";
#endif
    }

} /* namespace dataflow */

namespace dataflow
{
    void Analyzer::onAlloca(llvm::Value* target)
    {
#ifdef DBG_DUMP_INST
        llvm::errs() << "# alloca # target: " << LLVMUtil::GetVarDescript(target)
        << "\n";
#endif

        __var_addr addr(target, 0);
        _var_synonyms syn;
        syn.push_back(addr);
        _var_name name = LLVMUtil::GetVarName(target);
        _var v(name, syn);
        vars.push_back(v);
    }

    void Analyzer::onLoad(llvm::Value* target, llvm::Value* operand)
    {
#ifdef DBG_DUMP_INST
        llvm::errs() << "# load   # from  : " << LLVMUtil::GetVarDescript(operand)
        << "\n"
        << "           to    : " << LLVMUtil::GetVarDescript(target)
        << "\n";
#endif

        for(auto&& v : vars)
        {
            for(auto&& a : v.second)
            {
                if(a.first == operand)
                {
                    v.second.push_back(__var_addr(target, (v.second.end()-1)->second));
                    return;
                }
            }
        }
    }

    void Analyzer::onStore(llvm::Value* target, llvm::Value* operand)
    {
#ifdef DBG_DUMP_INST
        llvm::errs() << "# store  # from  : " << LLVMUtil::GetVarDescript(operand)
        << "\n"
        << "           to    : " << LLVMUtil::GetVarDescript(target)
        << "\n";
#endif

        for(auto&& v : vars)
        {
            for(auto&& a : v.second)
            {
                if(a.first == target)
                {
                    // it is saving modification, so __timeidx increases 1
                    v.second.push_back(__var_addr(operand, (v.second.end()-1)->second + 1));
                    if(v.first.empty())
                    {
                        v.first = LLVMUtil::GetVarName(operand);
                    }
                    return;
                }
            }
        }
    }

    char* fake_addr = 0;

    void Analyzer::onBinary(llvm::Value* block, llvm::Value* target, llvm::Value* left, llvm::Value* right)
    {
#ifdef DBG_DUMP_INST
        llvm::errs() << "# binary # left  : " << LLVMUtil::GetVarDescript(left)
        << "\n"
        << "           right : " << LLVMUtil::GetVarDescript(right)
        << "\n"
        << "           target: " << LLVMUtil::GetVarDescript(target)
        << "\n"
        << "           block : " << LLVMUtil::GetVarDescript(block)
        << "\n";
#endif

        _cond_type type = _cond_type::Common; // default is 0
        for(auto b = blocks.begin(); b != blocks.end(); ++b)
        {
            if(b->first == block)
            {
                type = b->second;
                blocks.erase(b);
                break;
            }
        }

        __var_addr addr((llvm::Value*)fake_addr, -1);
        _var_synonyms syn;
        syn.push_back(addr);
        _var_name name = LLVMUtil::GetVarDescript(target);
        _var v(name, syn);
        vars.push_back(v);

        __edge_link lnL(left, (llvm::Value*)fake_addr);
        __edge_link lnR(right, (llvm::Value*)fake_addr);
        __edge_type et(name, type);
        _edge L(lnL, et);
        _edge R(lnR, et);

        __edge_link lnT((llvm::Value*)fake_addr, target);
        __edge_type etT(name, _cond_type::Common);
        _edge T(lnT, etT);

        edges.push_back(L);
        edges.push_back(R);
        edges.push_back(T);

        ++fake_addr;
    }

    void Analyzer::onCall(llvm::Value* block, llvm::Value* result, std::vector<llvm::Value*> args)
    {
#ifdef DBG_DUMP_INST
        llvm::errs() << "# call   # result: " << LLVMUtil::GetVarDescript(result)
        << "\n";
        for(auto&& a : args)
        {

            llvm::errs() << "           arg   : " << LLVMUtil::GetVarDescript(a)
            <<"\n";
        }
#endif
        _cond_type type = _cond_type::Common; // default is 0
        for(auto b = blocks.begin(); b != blocks.end(); ++b)
        {
            if(b->first == block)
            {
                type = b->second;
                blocks.erase(b);
                break;
            }
        }

        __var_addr addr((llvm::Value*)(fake_addr++), -1);
        _var_synonyms syn;
        syn.push_back(addr);
        _var_name name = LLVMUtil::GetVarDescript(result);
        _var v(name, syn);
        vars.push_back(v);

        __edge_type inType(name, type);
        for(auto&& a : args)
        {
            __edge_link ln(a, (llvm::Value*)fake_addr);
            _edge L(ln, inType);
            edges.push_back(L);
        }

        __edge_type outType(name, _cond_type::Common);
        __edge_link lnT((llvm::Value*)fake_addr, result);
        _edge T(lnT, outType);
        edges.push_back(T);

        ++fake_addr;
    }

    void Analyzer::onBranch(llvm::Value* block, _cond_type type)
    {
        blocks.push_back(__basic_block(block, type));
    }
} /* namespace dataflow */
