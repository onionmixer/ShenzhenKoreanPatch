using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using Mono.Cecil;
using Mono.Cecil.Cil;
using MethodAttributes = Mono.Cecil.MethodAttributes;
using FieldAttributes = Mono.Cecil.FieldAttributes;

public static class BuildKoreanLocale
{
    private const string OriginalHash = "A0DFE8E1E91B6633C3BA00210762EC0D7E6786AB9C2C7912D9C9AFDD654D98F9";
    private const int Korean = 7;

    public static void Build(string original, string output, string runtimePath)
    {
        using (var hash = SHA256.Create())
        using (var input = File.OpenRead(original))
        {
            if (BitConverter.ToString(hash.ComputeHash(input)).Replace("-", "") != OriginalHash)
                throw new InvalidDataException("Unsupported original executable.");
        }

        Type runtime = Assembly.LoadFrom(runtimePath).GetType("KoreanLocaleRuntime", true);
        using (var assembly = AssemblyDefinition.ReadAssembly(original))
        {
            ModuleDefinition module = assembly.MainModule;
            TypeDefinition language = module.GetType("Language");
            language.Fields.Add(new FieldDefinition("Korean",
                FieldAttributes.Public | FieldAttributes.Static | FieldAttributes.Literal | FieldAttributes.HasDefault,
                language) { Constant = Korean });
            AddLanguageMappings(module);
            AddKoreanStrings(module, runtime);
            AddKoreanMessages(module, runtime);
            AddLanguageSelector(module, language);
            AddKoreanManual(module);
            assembly.Write(output);
        }
    }

    private static MethodDefinition Method(ModuleDefinition module, int token)
    {
        return (MethodDefinition)module.LookupToken(token);
    }

    private static Instruction At(MethodDefinition method, int offset)
    {
        return method.Body.Instructions.Single(instruction => instruction.Offset == offset);
    }

    private static void Before(ILProcessor il, Instruction at, params Instruction[] instructions)
    {
        foreach (Instruction instruction in instructions)
            il.InsertBefore(at, instruction);
    }

    private static void AddLanguageMappings(ModuleDefinition module)
    {
        MethodDefinition init = Method(module, 0x06000b2f);
        ILProcessor il = init.Body.GetILProcessor();
        int[] offsets = { 107, 249, 378 };
        string[] text = { "ko", "ko-KR", "\uD55C\uAD6D\uC5B4" };
        for (int index = 0; index < offsets.Length; index++)
        {
            Instruction store = At(init, offsets[index]);
            MethodReference add = (MethodReference)store.Previous.Operand;
            Before(il, store, il.Create(OpCodes.Dup), il.Create(OpCodes.Ldc_I4_7),
                il.Create(OpCodes.Ldstr, text[index]));
            if (index == 1)
                il.InsertBefore(store, il.Create(OpCodes.Newobj, (MethodReference)module.LookupToken(0x0a0004e2)));
            il.InsertBefore(store, il.Create(OpCodes.Callvirt, add));
        }
    }

    private static MethodDefinition CloneBody(MethodDefinition original, string name, TypeReference returnType)
    {
        if (original.Body.HasExceptionHandlers)
            throw new InvalidDataException("Unexpected localization loader shape.");
        var clone = new MethodDefinition(name, MethodAttributes.Private | MethodAttributes.Static, returnType);
        original.DeclaringType.Methods.Add(clone);
        clone.Body.InitLocals = true;
        foreach (ParameterDefinition parameter in original.Parameters)
            clone.Parameters.Add(new ParameterDefinition(parameter.ParameterType));
        foreach (VariableDefinition variable in original.Body.Variables)
            clone.Body.Variables.Add(new VariableDefinition(variable.VariableType));
        var mapping = original.Body.Instructions.ToDictionary(instruction => instruction,
            instruction => Instruction.Create(OpCodes.Nop));
        foreach (Instruction source in original.Body.Instructions)
        {
            Instruction copy = mapping[source];
            copy.Offset = source.Offset;
            copy.OpCode = source.OpCode;
            if (source.Operand is Instruction)
                copy.Operand = mapping[(Instruction)source.Operand];
            else if (source.Operand is Instruction[])
                copy.Operand = ((Instruction[])source.Operand).Select(value => mapping[value]).ToArray();
            else if (source.Operand is VariableDefinition)
                copy.Operand = clone.Body.Variables[((VariableDefinition)source.Operand).Index];
            else if (source.Operand is ParameterDefinition)
                copy.Operand = clone.Parameters[((ParameterDefinition)source.Operand).Index];
            else
                copy.Operand = source.Operand;
            clone.Body.Instructions.Add(copy);
        }
        return clone;
    }

    private static void AddKoreanStrings(ModuleDefinition module, Type runtime)
    {
        MethodDefinition stock = Method(module, 0x06000b30);
        var table = (FieldReference)module.LookupToken(0x04000a1a);
        MethodDefinition korean = CloneBody(stock, "LoadKoreanUiTable", table.FieldType);
        ILProcessor cloneIl = korean.Body.GetILProcessor();
        var localTable = new VariableDefinition(table.FieldType);
        korean.Body.Variables.Add(localTable);
        foreach (Instruction access in korean.Body.Instructions.Where(instruction => instruction.Operand == table).ToArray())
        {
            access.OpCode = access.OpCode == OpCodes.Stsfld ? OpCodes.Stloc : OpCodes.Ldloc;
            access.Operand = localTable;
        }
        Instruction file = At(korean, 20);
        file.OpCode = OpCodes.Ldstr;
        file.Operand = "strings.ko.csv";
        At(korean, 25).OpCode = OpCodes.Nop;
        At(korean, 25).Operand = null;
        At(korean, 263).OpCode = OpCodes.Ldc_I4_1;
        cloneIl.InsertBefore(At(korean, 316), cloneIl.Create(OpCodes.Ldloc, localTable));

        ILProcessor load = stock.Body.GetILProcessor();
        Instruction end = At(stock, 316);
        Before(load, end, load.Create(OpCodes.Ldstr, "Content/strings.ko.csv"),
            load.Create(OpCodes.Call, (MethodReference)module.LookupToken(0x0a00019f)),
            load.Create(OpCodes.Brfalse, end), load.Create(OpCodes.Ldsfld, table),
            load.Create(OpCodes.Call, korean),
            load.Create(OpCodes.Call, module.ImportReference(runtime.GetMethod("MergeUi"))));

        MethodDefinition get = Method(module, 0x06000b5b);
        ILProcessor read = get.Body.GetILProcessor();
        Instruction normal = get.Body.Instructions[0];
        var selected = (FieldReference)module.LookupToken(0x04000a19);
        var values = (FieldReference)module.LookupToken(0x04000a3e);
        var item = (MethodReference)At(get, 11).Operand;
        var contains = new MethodReference("ContainsKey", module.TypeSystem.Boolean, item.DeclaringType) { HasThis = true };
        contains.Parameters.Add(new ParameterDefinition(item.Parameters[0].ParameterType));
        Before(read, normal, read.Create(OpCodes.Ldsfld, selected), read.Create(OpCodes.Ldc_I4_7),
            read.Create(OpCodes.Bne_Un, normal), read.Create(OpCodes.Ldarg_0), read.Create(OpCodes.Ldfld, values),
            read.Create(OpCodes.Ldc_I4_7), read.Create(OpCodes.Callvirt, contains), read.Create(OpCodes.Brtrue, normal),
            read.Create(OpCodes.Ldarg_0), read.Create(OpCodes.Ldfld, values), read.Create(OpCodes.Ldc_I4_1),
            read.Create(OpCodes.Callvirt, item), read.Create(OpCodes.Ret));
    }

    private static void AddKoreanMessages(ModuleDefinition module, Type runtime)
    {
        MethodDefinition method = Method(module, 0x06000b7c);
        ILProcessor il = method.Body.GetILProcessor();
        foreach (int offset in new[] { 42, 352 })
            At(method, offset).OpCode = OpCodes.Ldc_I4_3;
        foreach (int offset in new[] { 56, 366 })
            Before(il, At(method, offset), il.Create(OpCodes.Dup), il.Create(OpCodes.Ldc_I4_2),
                il.Create(OpCodes.Ldc_I4_7), il.Create(OpCodes.Stelem_I4));
        var path = module.ImportReference(runtime.GetMethod("TextPath"));
        foreach (int offset in new[] { 126, 436 })
            il.InsertBefore(At(method, offset), il.Create(OpCodes.Call, path));
    }

    private static void Set(Instruction instruction, OpCode opcode, object operand = null)
    {
        instruction.OpCode = opcode;
        instruction.Operand = operand;
    }

    private static void AddKoreanManual(ModuleDefinition module)
    {
        MethodDefinition openManual = Method(module, 0x06000902);
        ILProcessor il = openManual.Body.GetILProcessor();
        Instruction join = At(openManual, 43);
        var platform = (FieldReference)join.Operand;
        var language = (FieldReference)module.LookupToken(0x04000a19);
        Instruction resume = il.Create(OpCodes.Ldsfld, platform);
        Set(join, OpCodes.Ldsfld, language);
        Instruction next = join.Next;
        Before(il, next, il.Create(OpCodes.Ldc_I4_7), il.Create(OpCodes.Bne_Un, resume),
            il.Create(OpCodes.Ldstr, "SHENZHEN IO Manual (Korean).pdf"),
            il.Create(OpCodes.Stloc_1), resume);
    }

    private static void AddLanguageSelector(ModuleDefinition module, TypeDefinition language)
    {
        MethodDefinition toggle = Method(module, 0x06000065);
        MethodDefinition chooser = CloneBody(toggle, "DrawLanguageChooser", language);
        chooser.Attributes = MethodAttributes.Assembly | MethodAttributes.Static;
        chooser.Parameters[0].ParameterType = language;
        chooser.Parameters.RemoveAt(3);
        chooser.Parameters.RemoveAt(2);
        chooser.Body.Variables[0].VariableType = language;
        chooser.Body.Variables[2].VariableType = language;

        var fromIndex = new MethodDefinition("LanguageAtIndex",
            MethodAttributes.Private | MethodAttributes.Static, language);
        chooser.DeclaringType.Methods.Add(fromIndex);
        fromIndex.Parameters.Add(new ParameterDefinition(module.TypeSystem.Int32));
        ILProcessor pick = fromIndex.Body.GetILProcessor();
        Instruction english = pick.Create(OpCodes.Ldc_I4_1);
        Instruction chinese = pick.Create(OpCodes.Ldc_I4_5);
        pick.Emit(OpCodes.Ldarg_0);
        pick.Emit(OpCodes.Switch, new[] { english, chinese });
        pick.Emit(OpCodes.Ldc_I4_7);
        pick.Emit(OpCodes.Ret);
        pick.Append(english);
        pick.Emit(OpCodes.Ret);
        pick.Append(chinese);
        pick.Emit(OpCodes.Ret);
        foreach (Instruction instruction in chooser.Body.Instructions.Where(instruction => instruction.Offset < 50))
            Set(instruction, OpCodes.Nop);
        Set(At(chooser, 68), OpCodes.Ldloc_1);
        Set(At(chooser, 69), OpCodes.Call, fromIndex);
        Set(At(chooser, 70), OpCodes.Nop);
        Set(At(chooser, 73), OpCodes.Ldsfld, module.LookupToken(0x04000a15));
        Set(At(chooser, 74), OpCodes.Ldloc_2);
        Set(At(chooser, 76), OpCodes.Callvirt, module.LookupToken(0x0a00048f));
        Set(At(chooser, 77), OpCodes.Call, Method(module, 0x060000a1));
        Set(At(chooser, 79), OpCodes.Nop);
        Set(At(chooser, 80), OpCodes.Nop);
        Set(At(chooser, 87), OpCodes.Ldc_I4, 120);
        Set(At(chooser, 289), OpCodes.Ldc_I4_3);

        MethodDefinition options = Method(module, 0x06000acf);
        ILProcessor il = options.Body.GetILProcessor();
        Instruction start = At(options, 405);
        Instruction skip = At(options, 544);
        var positionConstructor = (MethodReference)At(options, 420).Operand;
        var positionAdd = (MethodReference)At(options, 425).Operand;
        var selected = (VariableDefinition)At(options, 503).Operand;
        var languageField = (FieldReference)module.LookupToken(0x04000a19);
        var settingsInstance = (FieldReference)module.LookupToken(0x04000514);
        var languageOption = (FieldReference)module.LookupToken(0x040000cf);
        var modal = (FieldReference)module.LookupToken(0x0400049c);
        var setLanguage = (MethodReference)At(options, 517).Operand;
        foreach (Instruction instruction in options.Body.Instructions.Where(instruction => instruction.Offset >= 405 && instruction.Offset <= 524))
            Set(instruction, OpCodes.Nop);
        Before(il, start, il.Create(OpCodes.Ldloc_3), il.Create(OpCodes.Ldc_R4, 225f), il.Create(OpCodes.Ldc_R4, 20f),
            il.Create(OpCodes.Newobj, positionConstructor), il.Create(OpCodes.Call, positionAdd),
            il.Create(OpCodes.Call, chooser), il.Create(OpCodes.Stloc, selected), il.Create(OpCodes.Ldloc, selected),
            il.Create(OpCodes.Ldsfld, languageField), il.Create(OpCodes.Beq, skip),
            il.Create(OpCodes.Call, Method(module, 0x06000acb)), il.Create(OpCodes.Ldfld, modal),
            il.Create(OpCodes.Brtrue, skip), il.Create(OpCodes.Ldsfld, settingsInstance),
            il.Create(OpCodes.Ldfld, languageOption), il.Create(OpCodes.Ldloc, selected),
            il.Create(OpCodes.Callvirt, setLanguage), il.Create(OpCodes.Ldloc, selected),
            il.Create(OpCodes.Stsfld, languageField));
    }
}
