"""Compile and execute actual Ryujinx dominance/SSA and IR sources over tiny CFGs.

No alternate SSA implementation, shader decode, driver or game execution.
Requires local Roslyn compiler and .NET 8 runtime, not a full SDK install.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys

SOURCE_FILES = [
    'Translation/Ssa.cs', 'Translation/Dominance.cs', 'Translation/ControlFlowGraph.cs',
    'IntermediateRepresentation/BasicBlock.cs', 'IntermediateRepresentation/INode.cs',
    'IntermediateRepresentation/Operand.cs', 'IntermediateRepresentation/OperandHelper.cs',
    'IntermediateRepresentation/OperandType.cs', 'IntermediateRepresentation/PhiNode.cs',
    'IntermediateRepresentation/Operation.cs', 'IntermediateRepresentation/Instruction.cs',
    'IntermediateRepresentation/StorageKind.cs', 'IntermediateRepresentation/IrConsts.cs',
    'Decoders/Register.cs', 'Decoders/RegisterConsts.cs', 'Decoders/RegisterType.cs',
]

CSHARP = r'''
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Oracle {
    static object Run(string name, bool bodyFirst, bool reordered, int bodyValue, int expectedBackedge) {
        var e=new BasicBlock();var h=new BasicBlock();var b=new BasicBlock();var x=new BasicBlock();
        BasicBlock[] blocks=reordered?new[]{e,h,x,b}:new[]{e,h,b,x};
        for(int i=0;i<blocks.Length;i++)blocks[i].Index=i;
        var roles=new Dictionary<BasicBlock,string>{{e,"entry"},{h,"header"},{b,"body"},{x,"exit"}};
        e.Next=h;b.Next=h;h.Next=bodyFirst?b:x;h.Branch=bodyFirst?x:b;
        e.Append(new Operation(Instruction.Copy,Register(0,RegisterType.Gpr),Const(7)));
        var head=new Operation(Instruction.Copy,Local(),Register(0,RegisterType.Gpr));h.Append(head);
        b.Append(new Operation(Instruction.Copy,Register(0,RegisterType.Gpr),Const(bodyValue)));
        var exitRead=new Operation(Instruction.Copy,Local(),Register(0,RegisterType.Gpr));x.Append(exitRead);
        var cfg=new ControlFlowGraph(blocks);
        var postOrder=cfg.PostOrderBlocks.Select(q=>q.Index).ToArray();
        var pre=new List<object>();
        foreach(var q in blocks)pre.Add(new {role=roles[q],index=q.Index,preds=q.Predecessors.Select(k=>k.Index).ToArray(),next=q.Next?.Index,branch=q.Branch?.Index});
        // Exact production call order used by TranslatorContext.
        Dominance.FindDominators(cfg);Dominance.FindDominanceFrontiers(cfg.Blocks);Ssa.Rename(cfg.Blocks);
        var input=head.GetSource(0);var phi=input.AsgOp as PhiNode;
        var incoming=new List<object>();bool incomingCorrect=phi!=null&&phi.SourcesCount==2;
        if(phi!=null)for(int i=0;i<phi.SourcesCount;i++){
            var pred=phi.GetBlock(i);var v=ResolveCopyConstant(phi.GetSource(i));
            incoming.Add(new {predecessor=roles[pred],block_index=pred.Index,value=v,source_type=phi.GetSource(i).Type.ToString()});
            incomingCorrect &= (pred==e&&v==7)||(pred==b&&v==expectedBackedge);
        }
        bool exitSame=Object.ReferenceEquals(exitRead.GetSource(0),input);
        var dumps=new List<object>();
        var ids=new Dictionary<Operand,int>();
        string Id(Operand op){if(op==null)return "null";if(op.Type==OperandType.Constant)return "const:"+op.Value;if(op.Type==OperandType.Register)return "reg:"+op.GetRegister().Type+":"+op.GetRegister().Index;if(!ids.TryGetValue(op,out int id)){id=ids.Count;ids[op]=id;}return op.Type+":"+id;}
        foreach(var q in blocks)foreach(var node in q.Operations){
            var src=Enumerable.Range(0,node.SourcesCount).Select(i=>Id(node.GetSource(i))).ToArray();
            if(node is PhiNode p)dumps.Add(new {block=q.Index,role=roles[q],kind="Phi",dest=Id(node.Dest),sources=src,incoming_blocks=Enumerable.Range(0,p.SourcesCount).Select(p.GetBlock).Select(z=>z.Index).ToArray()});
            else dumps.Add(new {block=q.Index,role=roles[q],kind=((Operation)node).Inst.ToString(),dest=Id(node.Dest),sources=src});
        }
        return new {name,actual_backend="Ryujinx Dominance/ControlFlowGraph/Ssa.Rename",input_entry=7,input_backedge=bodyValue,expected_backedge=expectedBackedge,body_first_successor=bodyFirst,reordered_block_array=reordered,preSSA_CFG=pre,postorder=postOrder,reverse_postorder=postOrder.Reverse().ToArray(),dominators=blocks.Select(q=>new {block=q.Index,immediate_dominator=q.ImmediateDominator?.Index,frontier=q.DominanceFrontiers.Select(z=>z.Index).OrderBy(z=>z).ToArray()}).ToArray(),header_phi_incoming=incoming,header_phi_correct=incomingCorrect,exit_reuses_header_phi=exitSame,postSSA_IR=dumps,passed=incomingCorrect&&exitSame};
    }
    static int? ResolveCopyConstant(Operand value){
        var visited=new HashSet<Operand>();
        while(value!=null&&visited.Add(value)){
            if(value.Type==OperandType.Constant)return value.Value;
            if(value.AsgOp is Operation op&&op.Inst==Instruction.Copy&&op.SourcesCount==1)value=op.GetSource(0);else return null;
        }
        return null;
    }
    public static int Main(string[] args){
        try{
            int expect=args.Contains("--negative-expectation")?12:11;
            var cases=new[]{Run("loop_body_next",true,false,11,expect),Run("loop_exit_next",false,false,11,expect),Run("loop_reordered_blocks",true,true,11,expect),Run("loop_late_write_12",false,true,12,12)};
            var serialized=JsonSerializer.SerializeToElement(cases);
            bool pass=serialized.EnumerateArray().All(q=>q.GetProperty("passed").GetBoolean());
            Console.WriteLine(JsonSerializer.Serialize(new {schema=1,actual_production_source=true,alternate_SSA_algorithm=false,decode_lower_BuildASL_executed=false,GPU_executed=false,cases=cases,passed=pass}));
            return pass?0:1;
        }catch(Exception ex){Console.WriteLine(JsonSerializer.Serialize(new {schema=1,exception=ex.ToString(),passed=false}));return 2;}
    }
}
'''

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference-root',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--compiler',type=Path,default=Path(r'D:\VSBuildTools2022\MSBuild\Current\Bin\Roslyn\csc.exe'))
    ap.add_argument('--dotnet',type=Path,default=Path(r'C:\Program Files\dotnet\dotnet.exe'))
    ap.add_argument('--runtime-dir',type=Path,default=Path(r'C:\Program Files\dotnet\shared\Microsoft.NETCore.App\8.0.17'))
    ap.add_argument('--negative-expectation',action='store_true')
    a=ap.parse_args();root=a.reference_root.resolve(strict=True);out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    src=root/'src/Ryujinx.Graphics.Shader';sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    source={str(src/n):sha(src/n) for n in SOURCE_FILES};snapshot=out/'source-snapshot'
    for name in SOURCE_FILES:
        q=snapshot/name;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes((src/name).read_bytes())
    harness=out/'Oracle.cs';harness.write_text(CSHARP,encoding='utf-8');dll=out/'ryujinx-ssa-oracle.dll'
    names=['System.Private.CoreLib','System.Runtime','System.Collections','System.Console','System.Linq','System.Text.Json','System.Memory','System.Runtime.Extensions','System.Threading','System.Text.Encodings.Web']
    references=[a.runtime_dir/(n+'.dll') for n in names]
    args=[str(a.compiler),'/nologo','/noconfig','/nostdlib+','/langversion:latest','/target:exe','/platform:x64','/deterministic','/debug:portable','/optimize+','/out:'+str(dll),*[('/reference:'+str(q)) for q in references],str(harness),*[str(snapshot/n) for n in SOURCE_FILES]]
    build=subprocess.run(args,capture_output=True,text=True,timeout=60)
    (out/'BUILD_RESULT.json').write_text(json.dumps(dict(command=subprocess.list2cmdline(args),literal_stdout=build.stdout,literal_stderr=build.stderr,exit_status=build.returncode),indent=2))
    if build.returncode:
        print(build.stdout+build.stderr);return build.returncode
    (out/'ryujinx-ssa-oracle.runtimeconfig.json').write_text(json.dumps({'runtimeOptions':{'tfm':'net8.0','framework':{'name':'Microsoft.NETCore.App','version':a.runtime_dir.name}}}))
    native=[str(a.dotnet),'exec',str(dll)]
    if a.negative_expectation:native.append('--negative-expectation')
    r=subprocess.run(native,capture_output=True,text=True,timeout=30);payload=json.loads(r.stdout)
    expected_exit=1 if a.negative_expectation else 0
    assert r.returncode==expected_exit,(r.returncode,r.stdout,r.stderr)
    assert payload['actual_production_source'] and not payload['alternate_SSA_algorithm'] and len(payload['cases'])==4
    assert all(sha(Path(n))==h for n,h in source.items())
    head=subprocess.run(['git','-C',str(root),'rev-parse','HEAD'],capture_output=True,text=True,timeout=10)
    result=dict(command=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),input='4block CFG entryR0=7, header readR0, bodywrite11, exitreadR0; two successor orders, reordered dense block array, backedge12 variant; actual Ryujinx CFG/Dominance/SSA sources',source_root=str(root),source_HEAD=head.stdout.strip(),source_hashes=source,tool_hashes={str(a.compiler):sha(a.compiler),str(a.dotnet):sha(a.dotnet),str(harness):sha(harness)},runtime_references={str(q):sha(q) for q in references},assembly_sha256=sha(dll),build= json.loads((out/'BUILD_RESULT.json').read_text()),native_command=subprocess.list2cmdline(native),literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode,expected_exit=expected_exit,result=payload,not_game_input_or_GPU_execution=True)
    (out/'RESULT.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(f'RYUJINX_SSA_ORACLE_{"NEGATIVE_REJECTED" if a.negative_expectation else "PASS"}; actual_source=True; cases4=True; both_successor_orders=True; alternate_SSA=False; GPU_executed=False')
    return 0

if __name__=='__main__':raise SystemExit(main())
