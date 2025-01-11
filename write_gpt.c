#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

//++++++++++++++++++++++++++++++++++++++++++++++++++++
//Global Typedefs ++++++++++++++++++++++++++++++++++++
//++++++++++++++++++++++++++++++++++++++++++++++++++++

// Globally Unique IDentifier (a.k.a. UUID)
typedef struct {
  uint32_t time_lo;
  uint16_t time_mid;
  uint16_t time_hi_and_ver;             // Highest 4 bits are version #
  uint8_t clock_seq_hi_and_res;         // Highest bits are variant #
  uint8_t clock_seq_lo;
  uint8_t node[6];
}__attribute__((packed)) GUID;

//MBR Partition
typedef struct {
  uint8_t boot_indicator;
  uint8_t starting_chs[3];
  uint8_t os_type;
  uint8_t ending_chs[3];
  uint32_t starting_lba;
  uint32_t size_lba;
}__attribute__((packed)) Mbr_Partition;

//Master Boot Record  
typedef struct {
  uint8_t boot_code[440];
  uint32_t mbr_signature;
  uint16_t unknown;
  Mbr_Partition partition[4];
  uint16_t boot_signature;
}__attribute__((packed)) Mbr;

// GPT header
typedef struct {
  uint8_t signature[8];
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc32;
  uint32_t reserved_1;
  uint64_t my_lba;
  uint64_t alternate_lba;
  uint64_t first_usable_lba;
  uint64_t last_usable_lba;
  GUID disk_guid;
  uint64_t partition_table_lba;
  uint32_t number_of_entries;
  uint32_t size_of_entires;
  uint32_t partition_table_crc32;

  uint8_t reserved_2[512-92];
}__attribute__((packed)) Gpt_Header;

//++++++++++++++++++++++++++++++++++++++++++++++++++
//Global Variables +++++++++++++++++++++++++++++++++
//++++++++++++++++++++++++++++++++++++++++++++++++++
char *image_name = "test.img";
uint64_t lba_size = 512;
uint64_t esp_size = 1024*1024*33;   // 33 MiB
uint64_t data_size = 1024*1024*1;   // 1 MiB
uint64_t image_size = 0;
uint64_t esp_lbas, data_lbas, image_size_lbas;

//=================================================
//Global Funtions =================================
//=================================================

//-------------------------------------------------
// Pad out 0s to full lba size --------------------
//-------------------------------------------------
void write_full_lba_size(FILE *image){
  uint8_t zero_sector[512];
  for (uint8_t i = 0; i < (lba_size - sizeof zero_sector) / sizeof zero_sector; i++){
	fwrite(zero_sector, sizeof zero_sector, 1, image);
  }
}

//-------------------------------------------------
// Create a new Version 4 Variant 2 GUID ----------
//-------------------------------------------------
GUID new_guid(void){
  uint8_t rand_arr[16] = { 0 };

  for(uint8_t i = 0; i < sizeof rand_arr; i++){
	rand_arr[i] = rand() % (UINT8_MAX + 1);
  }

  // Fill out GUID
  GUID result = {
	.time_lo         = *(uint32_t *)&rand_arr[0],
	.time_mid        = *(uint16_t *)&rand_arr[4],
	.time_hi_and_ver = *(uint16_t *)&rand_arr[6],
	.clock_seq_hi_and_res = rand_arr[8],
	.clock_seq_lo = rand_arr[9],
	.node = { rand_arr[10], rand_arr[11], rand_arr[12],
	          rand_arr[13], rand_arr[14], rand_arr[15],
	},
  };

  // Fill out version bits
  //  result.time_hi_and_ver | 4;
  result.time_hi_and_ver &= ~(1 << 15);  // 0b_0_111 1111
  result.time_hi_and_ver |= (1 << 14);   // 0b0_1_00 0000
  result.time_hi_and_ver &= ~(1 << 13);  // 0b11_0_1 1111
  result.time_hi_and_ver &= ~(1 << 12);  // 0b111_0_ 1111
  
  // Fill out variant bits
  result.clock_seq_hi_and_res |= (1 << 7);       // 0b_1_000 0000
  result.clock_seq_hi_and_res |= (1 << 6);       // 0b0_1_00 0000
  result.clock_seq_hi_and_res &= ~(1 << 5);      // 0b11_0_1 1111  
  return result;
}

//-------------------------------------------------
// Create CRC32 Values ----------------------------
//-------------------------------------------------
uint32_t crc_table[256];

void create_crc32_table(void){
  uint32_t c;
  int32_t n, k;

  for (n = 0; n < 256; n++){
	c = (uint32_t) n;
	for(k = 0; k < 8; k++){
	  if(c & 1)
		c = 0xedb88320L ^ (c >> 1);
	  else
		c = c >> 1;
	}
	crc_table[n] = c;
  }
}

//-------------------------------------------------
// Calculate CRC32 value for range of data --------
//-------------------------------------------------
uint32_t calculate_crc32(void* buff, int32_t len){
  static bool made_crc_table = false;

  uint8_t *bufp = buff;
  uint32_t c = 0xFFFFFFFFL;
  int32_t n;

  if(!made_crc_table)
	create_crc32_table();

  for(n = 0; n < len; n++)
	c = crc_table[(c ^ bufp[n]) & 0xFF] ^ (c >> 8);

  // Invert bits for return value
  return c ^ 0xFFFFFFFFL;
}

//-------------------------------------------------
// Convert bytes to LBAS --------------------------
//-------------------------------------------------
uint64_t bytes_to_lbas(const uint64_t bytes){
  return (bytes / lba_size) + (bytes % lba_size > 0 ? 1 : 0);
}

//-------------------------------------------------
// Whrite protective MBR --------------------------
//-------------------------------------------------
bool write_mbr(FILE *image){
  uint64_t mbr_image_lbas = image_size_lbas;
  if(mbr_image_lbas > 0xFFFFFFFF) mbr_image_lbas = 0x100000000;
  Mbr mbr = {
	.boot_code = { 0 },
	.mbr_signature = 0,
	.unknown = 0,
	.partition[0] = {
	  .boot_indicator = 0,
	  .starting_chs = { 0x00, 0x02, 0x00 },
	  .os_type = 0xEE,    //Protective GPT
	  .ending_chs = { 0xFF, 0xFF, 0xFF },
	  .starting_lba = 0x00000001,
	  .size_lba = mbr_image_lbas - 1,
	},
	.boot_signature = 0xAA55,
  };

  //Whrite to file
  if(fwrite(&mbr, 1, sizeof mbr, image) != sizeof mbr)
	return false;
  return true;
}

//-------------------------------------------------
// Whrite GPT headers & tables, primary & secondary
//-------------------------------------------------
bool write_gpt(FILE *image){
  //TOBO:
  Gpt_Header primary_gpt = {
	.signature = { "EFI PART" },
	.revision = 0x00010000, //Version 1.0
	.header_size = 92,
	.header_crc32 = 0,      // Will calculate later
	.reserved_1 = 1,
	.my_lba = 1,            // LBA 1 is right after MBR
	.alternate_lba = image_size_lbas - 1,
	.first_usable_lba = 1 + 1 + 32, // MBR + GPT + primary GPT table
	.last_usable_lba = image_size_lbas - 1 - 32 - 1, // 2nd GPT header + table 
	.disk_guid = new_guid(),
	.partition_table_lba = 2, // After MBR + GPT header
	.number_of_entries = 128,
	.size_of_entires = 128,
	.partition_table_crc32 = 0, // Will calculate later

	.reserved_2 = { 0 },
  };
  
	
  return true;
}

//=================================================
//MAIN ============================================
//=================================================
int main(void){
  FILE *image = fopen(image_name, "wb+");

  if(!image){
	fprintf(stderr, "Error: could not open file %s\n", image_name);
	return EXIT_FAILURE;
  }

  //Set sizes
  image_size = esp_size + data_size + (1024*1024); // Add some extra padding for GPTs/MBR

  image_size_lbas = bytes_to_lbas(image_size);

  // Seed random number generation
  srand(time(NULL));

  //Writre protective MBR
  if(!write_mbr(image)){
	fprintf(stderr, "Error: could not open protective MBR for file %s\n", image_name);
	return EXIT_FAILURE;	
  }

  //Writre GPT headers & tables
  if(!write_gpt(image)){
	fprintf(stderr, "Error: could not write GPT headers & tables for file %s\n", image_name);
	return EXIT_FAILURE;	
  }
  
  write_full_lba_size(image);
  return EXIT_SUCCESS;
}
