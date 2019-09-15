using System;
using System.Collections.Generic;
using System.Text;
using Tangle.Net.Entity;

namespace EHRIoTFunctionApp
{
    public static class NationalIdentityService
    {
        static string s_sampleNprId1 = "20054802316";
        static string s_sampleNprId2 = "21099717960";

        static Seed s_sampleNprSeed1 = new Seed("QOXOWBFMSZKXNLHZWDTPUQTW99OSPFHSLEGAQMR9FDSLYUPGCMZBGCEXCN9BBW9CBNHNQACFHYJEQOU99"); //iotaEHR1
        static Seed s_sampleNprSeed2 = new Seed("JVCG9MTH99GSARLVOS9OGPIJDV99OGFELIRIMZBGJMERNHMPCTFQFZOPVXXKYO9KDKLUKYWXRXT9SUFSR"); //iotaEHR2

        static Dictionary<string, Seed> s_nationalPatientRegistrySeedVault = new Dictionary<string, Seed> { [s_sampleNprId1] = s_sampleNprSeed1, [s_sampleNprId2] = s_sampleNprSeed2 };

        public static Seed GetSeedFromNprId(string nprId)
        {
            if (!s_nationalPatientRegistrySeedVault.ContainsKey(nprId))
            {
                return null;
            }

            return s_nationalPatientRegistrySeedVault[nprId];
        }
    }
}
