using System;
using System.Collections;
using System.IO;
using System.Reflection;

public static class KoreanLocaleRuntime
{
    public static void MergeUi(IDictionary original, IDictionary korean)
    {
        FieldInfo values = null;
        object chineseKey = null;
        object koreanKey = null;
        foreach (DictionaryEntry entry in korean)
        {
            if (!original.Contains(entry.Key))
                continue;

            if (values == null)
            {
                foreach (FieldInfo field in entry.Value.GetType().GetFields(
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
                {
                    if (typeof(IDictionary).IsAssignableFrom(field.FieldType))
                    {
                        values = field;
                        break;
                    }
                }
                if (values == null)
                    throw new InvalidOperationException("Localized values field was not found.");
                Type language = values.FieldType.GetGenericArguments()[0];
                chineseKey = Enum.ToObject(language, 5);
                koreanKey = Enum.ToObject(language, 7);
            }

            IDictionary incoming = (IDictionary)values.GetValue(entry.Value);
            IDictionary destination = (IDictionary)values.GetValue(original[entry.Key]);
            if (incoming.Contains(chineseKey))
                destination[koreanKey] = incoming[chineseKey];
        }
    }

    public static string TextPath(string path)
    {
        string directory = Path.GetDirectoryName(path);
        if (directory == null || !directory.EndsWith(".ko", StringComparison.Ordinal) || File.Exists(path))
            return path;

        return Path.Combine(directory.Substring(0, directory.Length - 3) + ".en", Path.GetFileName(path));
    }
}
