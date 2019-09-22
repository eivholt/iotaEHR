using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Azure.WebJobs;
using Microsoft.Azure.WebJobs.Extensions.Http;
using Microsoft.Extensions.Logging;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using RestSharp;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Tangle.Net.Cryptography;
using Tangle.Net.Entity;
using Tangle.Net.Repository;
using Tangle.Net.Utils;

namespace EHRIoTFunctionApp
{
    public static class PublishReading
    {
        [FunctionName("PublishReading")]
        public static async Task<IActionResult> Run(
            [HttpTrigger(AuthorizationLevel.Function, "post", Route = null)] HttpRequest req,
            ILogger log)
        {
            log.LogInformation("C# HTTP trigger function processed a request.");

            // Get needed parameters from payload.
            string requestBody = await new StreamReader(req.Body).ReadToEndAsync();
            dynamic data = JsonConvert.DeserializeObject(requestBody);
            string deviceId = data?.device.deviceId;
            dynamic telemetry = data?.device.measurements.telemetry;
            dynamic properties = data?.device.properties;
            string nprId = properties.device.nprId_property.ToString();

            // Basic validation before proceeding.
            if (string.IsNullOrEmpty(nprId) || !(telemetry is JObject))
            {
                return new BadRequestObjectResult("NprId not set on reporting device or reading invalid.");
            }

            // Connect to a node on one of the main networks.
            var repository = new RestIotaRepository(new RestClient("https://nodes.devnet.thetangle.org:443"));

            // Get addresses from the Tangle on behalf of the patient's wallet for generating new address for posting reading. 
            // Would be an external trusted service.
            var tangleAddresses = NationalIdentityService.GetTangleAddressesFromNprId(repository, nprId, ((JObject)telemetry).Count);

            // Terminate if no addresses created.
            if (tangleAddresses.Count < 1)
            {
                return new BadRequestObjectResult("NprId not found or no associated seed.");
            }

            // Create transactions for the Tangle for each type of reading in payload and bundle together.
            // Finalize and sign.
            var bundle = CreateTransaction(tangleAddresses, telemetry);

            // Send the complete transactions to the Tangle.
            repository.SendTrytes(bundle.Transactions, depth: 2, minWeightMagnitude: 9);

            string logMessage = $"NPRId: {nprId} - DeviceID: {deviceId} - reading: {telemetry.ToString()} - Bundle hash: {bundle.Hash.Value}";
            log.LogInformation(logMessage);

            return (ActionResult)new OkObjectResult(logMessage);
        }

        private static Bundle CreateTransaction(List<Address> addresses, dynamic telemetry)
        {
            // Iterate over the empty transactions and add a light-weight openEHR payload.
            int currentAddress = 0;
            var bundle = new Bundle();

            // SpO2 reading.
            if (!string.IsNullOrEmpty(telemetry.SpO2.ToString()))
            {
                var spO2Payload = CreateArchetypeObservationFromTemplate(
                    s_pulse_oximetryArchetypeId,
                    s_pulse_oximetryObservationCodeId,
                    s_pulse_oximetryValueCodeId,
                    telemetry.SpO2.ToString());

                CreateTransferAndAddToBundle(addresses[currentAddress++], bundle, spO2Payload);
            }

            // Heart rate reading.
            if (!string.IsNullOrEmpty(telemetry.Heart_rate.ToString()))
            {
                var heartRatePayload = CreateArchetypeObservationFromTemplate(
                    s_pulseArchetypeId,
                    s_pulseObservationCodeId,
                    s_pulseValueCodeId,
                    telemetry.Heart_rate.ToString());

                CreateTransferAndAddToBundle(addresses[currentAddress++], bundle, heartRatePayload);
            }

            bundle.Finalize();
            bundle.Sign();
            return bundle;
        }

        private static void CreateTransferAndAddToBundle(Address address, Bundle bundle, dynamic payload)
        {
            bundle.AddTransfer(new Transfer
            {
                Address = new Address(address.Value),
                Tag = new Tag("OPENEHR"),
                Timestamp = Timestamp.UnixSecondsTimestamp,
                Message = TryteString.FromUtf8String(payload.ToString())
            });
        }

        // Hard-coded creation of a light-weight openEHR Observation. Would be replaced by a Template repository.
        private static dynamic CreateArchetypeObservationFromTemplate(string archetypeId, string observationCodeId, string valueCodeId, string magnitude)
        {
            dynamic payload = new JObject();
            payload.archetype_node_id = archetypeId;
            payload.events = new JObject();
            payload.events.archetype_node_id = observationCodeId;
            payload.events.data = new JObject();
            payload.events.data.archetype_node_id = valueCodeId;
            payload.events.data.value = new JObject();
            payload.events.data.value.magnitude = magnitude;

            return payload;
        }

        private static string s_pulse_oximetryArchetypeId = "openEHR-EHR-OBSERVATION.pulse_oximetry.v1";
        private static string s_pulse_oximetryObservationCodeId = "at0000"; // Pulse oximetry
        private static string s_pulse_oximetryValueCodeId = "at0006"; // SpO₂

        private static string s_pulseArchetypeId = "openEHR-EHR-OBSERVATION.pulse.v1";
        private static string s_pulseObservationCodeId = "at0000"; // Pulse/Heart beat
        private static string s_pulseValueCodeId = "at0004"; // Rate
    }
}
