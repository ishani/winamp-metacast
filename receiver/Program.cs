using System;
using System.Collections.Generic;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Reflection;
using System.Security.Principal;
using System.Threading;

using Newtonsoft.Json;

namespace receiver
{
    // matching the JSON naming from the Winamp plugin
    [Serializable]
    public class WinampMetadata
    {
        public int playlistIndex   { get; set; }
        public string trackNum     { get; set; }
        public string discNum      { get; set; }
        public string artist       { get; set; }
        public string title        { get; set; }
        public string album        { get; set; }
        public string year         { get; set; }
        public string BPM          { get; set; }
        public string length       { get; set; }
        public string album_artist { get; set; }
        public string path         { get; set; }
        public bool isPlaying      { get; set; }
    }

    // some basic configuration, loaded from json - a list of magic replacement strings and a file output, eg
    //  {
    //      "Configs" : 
    //      [
    //          {
    //              "Formatting" : "%discNum%:%trackNum% - %title%",
    //              "FileTarget" : "F:\\now_playing_1.txt"
    //          },
    //          {
    //              "Formatting" : "%artist%",
    //              "FileTarget" : "F:\\now_playing_2.txt"
    //          }
    //      ]
    //  }
    //
    //
    [Serializable]
    public class OutputConfig
    {
        public string Formatting   { get; set; }
        public string FileTarget   { get; set; }
    }

    [Serializable]
    public class OutputConfigs
    {
        public List<OutputConfig> Configs { get; set; }
    }

    class Program
    {
        static private readonly SecurityIdentifier securityWorldSID = new SecurityIdentifier( WellKnownSidType.WorldSid , null );

        // matching identifiers in the GlobalTransmission class in the Winamp plugin
        private static readonly string cGlobalMapppingName       = "WAIshani_Metacast";
        private static readonly string cGlobalMutexName          = "Global\\WAIshani_MetacastMutex";
        private static readonly Int32  cSharedBufferSize         = 1024 * 4;

        private static MemoryMappedFile mmFile;
        private static MemoryMappedViewStream mmStream;

        private static void SetupMemoryMapFile()
        {
            var security = new MemoryMappedFileSecurity();

            security.SetAccessRule(
                new System.Security.AccessControl.AccessRule<MemoryMappedFileRights>( securityWorldSID,
                    MemoryMappedFileRights.FullControl,
                    System.Security.AccessControl.AccessControlType.Allow ) );

            mmFile = MemoryMappedFile.CreateOrOpen( cGlobalMapppingName,
                    cSharedBufferSize,
                    MemoryMappedFileAccess.ReadWrite,
                    MemoryMappedFileOptions.None,
                    security,
                    HandleInheritability.Inheritable );

            mmStream = mmFile.CreateViewStream( 0, cSharedBufferSize );
        }

        private static readonly byte[] transmissionBuffer = new byte[cSharedBufferSize];


        // simple config scheme - read in a json of outputs from next to wherever we're running
        private static readonly string configFileName = "config.json";

        // a prepared list of %metadata_name% -> reflected property, used when processing strings eg "%discNum%:%trackNum% - %title%"
        private static Dictionary<string, PropertyInfo> metadataReplacements = new Dictionary<string, PropertyInfo>();


        // produces outputs for all defined configs using the given metadata info
        static void Execute( WinampMetadata metadata, OutputConfigs outputs )
        {
            foreach ( var cfg in outputs.Configs )
            {
                string result = cfg.Formatting;
                foreach ( var replacement in metadataReplacements )
                {
                    result = result.Replace( replacement.Key, replacement.Value.GetValue( metadata ) as string );
                }
                System.IO.File.WriteAllText( cfg.FileTarget, result );
            }
        }

        static void Main( string[] args )
        {
            if ( !File.Exists( configFileName ) )
            {
                Console.WriteLine( $"Cannot find configuration file [{configFileName}]" );
                return;
            }

            string configData = File.ReadAllText( "config.json" );
            if ( string.IsNullOrEmpty(configData) )
            {
                Console.WriteLine( $"Config file [{configFileName}] empty" );
                return;
            }

            OutputConfigs cfg;
            try
            {
                cfg = JsonConvert.DeserializeObject<OutputConfigs>( configData );
            }
            catch ( Exception ex )
            {
                Console.WriteLine( $"Config file deserialize failed, {ex.Message}" );
                return;
            }

            PropertyInfo[] properties = typeof(WinampMetadata).GetProperties();
            foreach ( PropertyInfo property in properties )
            {
                metadataReplacements.Add( $"%{property.Name}%", property );
            }


            // plug into the matrix
            SetupMemoryMapFile();

            bool wasNew = false;
            var globalMutex = new Mutex( false, cGlobalMutexName, out wasNew );

            for ( ; ; )
            {
                if ( globalMutex.WaitOne(500) )
                {
                    try
                    {
                        Int32 readLen = mmStream.Read( transmissionBuffer, 0, cSharedBufferSize );
                        mmStream.Seek( 0, SeekOrigin.Begin );

                        int transmissionID = BitConverter.ToInt32(transmissionBuffer, 0);
                        int stringLength   = BitConverter.ToInt32(transmissionBuffer, 4);

                        bool recvOK = (transmissionID & 0xFF000000) == 0x6A000000;
                        transmissionID &= 0x00FFFFFF;

                        Console.WriteLine( $"------------ sync:{recvOK} @ {transmissionID} --" );

                        if ( recvOK && stringLength != 0 )
                        {
                            string result = System.Text.Encoding.ASCII.GetString( transmissionBuffer, 8, stringLength );

                            WinampMetadata meta = JsonConvert.DeserializeObject<WinampMetadata>(result);

                            // uncomment to check/see the data in the console
                            // Console.WriteLine( JsonConvert.SerializeObject( meta, Formatting.Indented ) );

                            Execute( meta, cfg );
                        }
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine( $"ERROR: {ex.Message}" );
                    }
                    finally
                    {
                        globalMutex.ReleaseMutex();
                    }
                }

                Thread.Sleep( 5000 );
                Console.WriteLine( "." );
            }
        }
    }
}
