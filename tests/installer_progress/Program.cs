using System.Collections;
using System.Reflection;
using System.Runtime.CompilerServices;

// Exercise the production log parser without opening a window or starting setup.
var formType = Assembly.Load("OpenSmash").GetType("OpenSmash.Launcher.InstallerForm", true)!;
var stepType = formType.GetNestedType("Step", BindingFlags.NonPublic)!;
var form = RuntimeHelpers.GetUninitializedObject(formType);
var stepsField = formType.GetField("_steps", BindingFlags.Instance | BindingFlags.NonPublic)!;
var steps = (IList)Activator.CreateInstance(stepsField.FieldType)!;
stepsField.SetValue(form, steps);
var step = Activator.CreateInstance(stepType, true)!;
void Set(string name, object value) => stepType.GetField(name)!.SetValue(step, value);
T Get<T>(string name) => (T)stepType.GetField(name)!.GetValue(step)!;
void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
void Line(string text) => formType.GetMethod("ApplyMeasure", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form, new object[] { text });
Set("State", "run");
Set("Done", 65234d);
Set("Total", 65234d);
Set("Msg", "compiling shaders");
steps.Add(step);
Line("warm: drawing the cache once through Direct3D so the first use does not compile it");
Check(Get<bool>("Warming"), "warm phase not recognized");
Check(Get<double>("Total") == 0 && Get<double>("Done") == 0, "stale 100% compilation count retained");
Check(Get<string>("Msg").Contains("graphics-driver"), "warm-up label missing");
Line("[warm] 512/65228 processed (0 failed), elapsed 5 s; time remaining unknown");
Check(Get<double>("Done") == 512 && Get<double>("Total") == 65228, "warm progress not parsed");
Line("[warm] 65228/65228 processed (0 failed), elapsed 50 s; finishing driver-cache writes");
Check(Get<string>("Msg") == "finishing driver-cache writes", "final cache-writing phase missing");
Console.WriteLine("Installer warm-up phase, progress, and finalization checks passed.");
