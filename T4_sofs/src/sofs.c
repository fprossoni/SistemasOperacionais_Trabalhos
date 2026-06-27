/*
 * sofs.c - Implementação (esqueleto) do sistema de arquivos sofs.
 *
 * A camada de blocos (sofs-block) é usada para todos os acessos ao disco;
 * a camada de bitmap (bitmap2) gerencia o controle de blocos e i-nodes livres.
 *
 * Layout do sistema de arquivos dentro de uma partição (em ordem):
 *   [bloco 0]          superbloco
 *   [blocos 1 .. bb]   bitmap de blocos livres   (bb = freeBlocksBitmapSize)
 *   [bb+1 .. bb+bi]    bitmap de i-nodes livres  (bi = freeInodeBitmapSize)
 *   [bb+bi+1 .. ...]   área de i-nodes           (10% dos blocos, arredondado para cima)
 *   [resto]            blocos de dados
 *
 * As funções marcadas com TODO são responsabilidade do grupo.
 * As funções auxiliares alloc_data_block(), free_data_block(),
 * alloc_inode() e free_inode() são fornecidas como blocos de construção.
 */

#include <string.h>
#include <stdlib.h>
#include "sofs.h"
#include "sofs-block.h"

/* -------------------------------------------------------------------------
 * Estado interno de montagem
 * ---------------------------------------------------------------------- */

static int g_mounted = false;
static struct sofs_superbloco g_superbloco;
static unsigned int g_superbloco_sector;   /* setor absoluto do superbloco */

/* Protótipos das funções auxiliares fornecidas */
static int alloc_data_block(void);
static int free_data_block(unsigned int abs_block_num);
static int alloc_inode(void);
static int free_inode(unsigned int inode_num);

/* -------------------------------------------------------------------------
 * Estado e Estruturas para Gerência de Arquivos e Diretórios Abertos
 * ---------------------------------------------------------------------- */

static int g_dir_open = false;
static unsigned int g_dir_curr_record = 0;

struct open_file_entry {
    int in_use;
    unsigned int inode_num;
    unsigned int curr_pos;
};

static struct open_file_entry g_open_files[10];

static void check_and_init_open_files(void) {
    static int was_mounted = false;
    if (g_mounted) {
        if (!was_mounted) {
            for (int i = 0; i < 10; i++) {
                g_open_files[i].in_use = false;
                g_open_files[i].inode_num = 0;
                g_open_files[i].curr_pos = 0;
            }
            g_dir_open = false;
            g_dir_curr_record = 0;
            was_mounted = true;
        }
    } else {
        was_mounted = false;
    }
}

static int is_valid_handle(SOFS_FILE handle) {
    check_and_init_open_files();
    if (handle < 0 || handle >= 10)
        return false;
    return g_open_files[handle].in_use;
}

/* -------------------------------------------------------------------------
 * Funções Auxiliares: E/S de i-nodes e Resolução de Ponteiros
 * ---------------------------------------------------------------------- */

static int read_inode(unsigned int inode_num, struct sofs_inode *inode) {
    check_and_init_open_files();
    if (!g_mounted || inode == NULL)
        return -1;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int inodes_per_block = block_size / sizeof(struct sofs_inode);
    unsigned int inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + inode_num / inodes_per_block;
    unsigned int inode_offset = inode_num % inodes_per_block;

    unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0)
        return -1;

    memcpy(inode, buf + inode_offset * sizeof(struct sofs_inode), sizeof(struct sofs_inode));
    return 0;
}

static int write_inode(unsigned int inode_num, const struct sofs_inode *inode) {
    check_and_init_open_files();
    if (!g_mounted || inode == NULL)
        return -1;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int inodes_per_block = block_size / sizeof(struct sofs_inode);
    unsigned int inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + inode_num / inodes_per_block;
    unsigned int inode_offset = inode_num % inodes_per_block;

    unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0)
        return -1;

    memcpy(buf + inode_offset * sizeof(struct sofs_inode), inode, sizeof(struct sofs_inode));
    if (write_block(inode_block, buf) != 0)
        return -1;
    return 0;
}

static int get_physical_block(const struct sofs_inode *inode, unsigned int file_block) {
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    if (file_block < 2) {
        return (int)inode->dataPtr[file_block];
    }
    
    unsigned int ptrs_per_block = block_size / sizeof(DWORD);
    unsigned int check_block = file_block - 2;
    if (check_block < ptrs_per_block) {
        if (inode->singleIndPtr == 0) return 0;
        unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
        if (read_block(inode->singleIndPtr, buf) != 0) return -1;
        DWORD *ptrs = (DWORD *)buf;
        return (int)ptrs[check_block];
    }
    
    check_block -= ptrs_per_block;
    unsigned int double_limit = ptrs_per_block * ptrs_per_block;
    if (check_block < double_limit) {
        if (inode->doubleIndPtr == 0) return 0;
        unsigned char *buf1 = (unsigned char *)__builtin_alloca(block_size);
        if (read_block(inode->doubleIndPtr, buf1) != 0) return -1;
        DWORD *ptrs1 = (DWORD *)buf1;
        
        unsigned int outer_idx = check_block / ptrs_per_block;
        unsigned int inner_idx = check_block % ptrs_per_block;
        
        DWORD single_ptr = ptrs1[outer_idx];
        if (single_ptr == 0) return 0;
        
        unsigned char *buf2 = (unsigned char *)__builtin_alloca(block_size);
        if (read_block(single_ptr, buf2) != 0) return -1;
        DWORD *ptrs2 = (DWORD *)buf2;
        return (int)ptrs2[inner_idx];
    }
    
    return -1;
}

static int read_directory_record(unsigned int record_idx, struct sofs_record *record) {
    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    if (record_idx >= total_records)
        return -1;

    unsigned int byte_offset = record_idx * sizeof(struct sofs_record);
    unsigned int file_block = byte_offset / block_size;
    unsigned int block_offset = byte_offset % block_size;

    int phys_block = get_physical_block(&root_inode, file_block);
    if (phys_block <= 0)
        return -1;

    unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block((unsigned int)phys_block, buf) != 0)
        return -1;

    memcpy(record, buf + block_offset, sizeof(struct sofs_record));
    return 0;
}

static int write_directory_record(unsigned int record_idx, const struct sofs_record *record) {
    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    if (record_idx >= total_records)
        return -1;

    unsigned int byte_offset = record_idx * sizeof(struct sofs_record);
    unsigned int file_block = byte_offset / block_size;
    unsigned int block_offset = byte_offset % block_size;

    int phys_block = get_physical_block(&root_inode, file_block);
    if (phys_block <= 0)
        return -1;

    unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block((unsigned int)phys_block, buf) != 0)
        return -1;

    memcpy(buf + block_offset, record, sizeof(struct sofs_record));
    if (write_block((unsigned int)phys_block, buf) != 0)
        return -1;
    return 0;
}

static int find_file_inode(const char *name, unsigned int *inode_num, BYTE *out_type, int depth) {
    if (depth > 5)
        return -1;

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    for (unsigned int idx = 0; idx < total_records; idx++) {
        struct sofs_record rec;
        if (read_directory_record(idx, &rec) == 0 && rec.TypeVal != TYPEVAL_INVALIDO) {
            if (strcmp(rec.name, name) == 0) {
                if (rec.TypeVal == TYPEVAL_LINK) {
                    struct sofs_inode link_inode;
                    if (read_inode(rec.inodeNumber, &link_inode) != 0)
                        return -1;
                    if (link_inode.dataPtr[0] == 0)
                        return -1;
                    
                    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
                    unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
                    if (read_block(link_inode.dataPtr[0], buf) != 0)
                        return -1;
                    
                    return find_file_inode((char *)buf, inode_num, out_type, depth + 1);
                } else {
                    *inode_num = rec.inodeNumber;
                    if (out_type) *out_type = rec.TypeVal;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int find_or_allocate_directory_slot(const char *name, unsigned int inode_num) {
    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    
    int free_slot = -1;
    for (unsigned int idx = 0; idx < total_records; idx++) {
        struct sofs_record rec;
        if (read_directory_record(idx, &rec) == 0) {
            if (rec.TypeVal == TYPEVAL_INVALIDO) {
                free_slot = (int)idx;
                break;
            }
        }
    }

    if (free_slot != -1) {
        struct sofs_record new_rec;
        memset(&new_rec, 0, sizeof(new_rec));
        new_rec.TypeVal = TYPEVAL_REGULAR;
        strncpy(new_rec.name, name, 50);
        new_rec.name[50] = '\0';
        new_rec.inodeNumber = inode_num;
        if (write_directory_record((unsigned int)free_slot, &new_rec) != 0)
            return -1;
        return free_slot;
    }

    unsigned int next_file_block = root_inode.blocksFileSize;
    int new_block = alloc_data_block();
    if (new_block < 0)
        return -1;

    if (next_file_block < 2) {
        root_inode.dataPtr[next_file_block] = (DWORD)new_block;
    } else {
        unsigned int ptrs_per_block = block_size / sizeof(DWORD);
        unsigned int check_block = next_file_block - 2;
        if (check_block < ptrs_per_block) {
            if (root_inode.singleIndPtr == 0) {
                int ind_block = alloc_data_block();
                if (ind_block < 0) {
                    free_data_block((unsigned int)new_block);
                    return -1;
                }
                root_inode.singleIndPtr = (DWORD)ind_block;
            }
            unsigned char *ind_buf = (unsigned char *)__builtin_alloca(block_size);
            if (read_block(root_inode.singleIndPtr, ind_buf) != 0) {
                free_data_block((unsigned int)new_block);
                return -1;
            }
            DWORD *ptrs = (DWORD *)ind_buf;
            ptrs[check_block] = (DWORD)new_block;
            if (write_block(root_inode.singleIndPtr, ind_buf) != 0) {
                free_data_block((unsigned int)new_block);
                return -1;
            }
        } else {
            free_data_block((unsigned int)new_block);
            return -1;
        }
    }

    root_inode.blocksFileSize++;
    root_inode.bytesFileSize += block_size;
    if (write_inode(0, &root_inode) != 0) {
        free_data_block((unsigned int)new_block);
        return -1;
    }

    unsigned int new_record_idx = total_records;
    struct sofs_record new_rec;
    memset(&new_rec, 0, sizeof(new_rec));
    new_rec.TypeVal = TYPEVAL_REGULAR;
    strncpy(new_rec.name, name, 50);
    new_rec.name[50] = '\0';
    new_rec.inodeNumber = inode_num;
    if (write_directory_record(new_record_idx, &new_rec) != 0)
        return -1;

    return (int)new_record_idx;
}

static void truncate_file_blocks(struct sofs_inode *inode) {
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    
    for (int i = 0; i < 2; i++) {
        if (inode->dataPtr[i] != 0) {
            free_data_block(inode->dataPtr[i]);
            inode->dataPtr[i] = 0;
        }
    }
    
    if (inode->singleIndPtr != 0) {
        unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
        if (read_block(inode->singleIndPtr, buf) == 0) {
            DWORD *ptrs = (DWORD *)buf;
            unsigned int ptrs_per_block = block_size / sizeof(DWORD);
            for (unsigned int i = 0; i < ptrs_per_block; i++) {
                if (ptrs[i] != 0) {
                    free_data_block(ptrs[i]);
                }
            }
        }
        free_data_block(inode->singleIndPtr);
        inode->singleIndPtr = 0;
    }
    
    if (inode->doubleIndPtr != 0) {
        unsigned char *buf1 = (unsigned char *)__builtin_alloca(block_size);
        if (read_block(inode->doubleIndPtr, buf1) == 0) {
            DWORD *ptrs1 = (DWORD *)buf1;
            unsigned int ptrs_per_block = block_size / sizeof(DWORD);
            for (unsigned int i = 0; i < ptrs_per_block; i++) {
                if (ptrs1[i] != 0) {
                    unsigned char *buf2 = (unsigned char *)__builtin_alloca(block_size);
                    if (read_block(ptrs1[i], buf2) == 0) {
                        DWORD *ptrs2 = (DWORD *)buf2;
                        for (unsigned int j = 0; j < ptrs_per_block; j++) {
                            if (ptrs2[j] != 0) {
                                free_data_block(ptrs2[j]);
                            }
                        }
                    }
                    free_data_block(ptrs1[i]);
                }
            }
        }
        free_data_block(inode->doubleIndPtr);
        inode->doubleIndPtr = 0;
    }
    
    inode->blocksFileSize = 0;
    inode->bytesFileSize = 0;
}

static int allocate_file_block(struct sofs_inode *inode, unsigned int inode_num, unsigned int file_block) {
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    
    int existing = get_physical_block(inode, file_block);
    if (existing > 0)
        return existing;
    if (existing < 0)
        return -1;

    int new_block = alloc_data_block();
    if (new_block < 0)
        return -1;

    if (file_block < 2) {
        inode->dataPtr[file_block] = (DWORD)new_block;
    } else {
        unsigned int ptrs_per_block = block_size / sizeof(DWORD);
        unsigned int check_block = file_block - 2;
        
        if (check_block < ptrs_per_block) {
            if (inode->singleIndPtr == 0) {
                int ind_block = alloc_data_block();
                if (ind_block < 0) {
                    free_data_block((unsigned int)new_block);
                    return -1;
                }
                inode->singleIndPtr = (DWORD)ind_block;
            }
            unsigned char *ind_buf = (unsigned char *)__builtin_alloca(block_size);
            if (read_block(inode->singleIndPtr, ind_buf) != 0) {
                free_data_block((unsigned int)new_block);
                return -1;
            }
            DWORD *ptrs = (DWORD *)ind_buf;
            ptrs[check_block] = (DWORD)new_block;
            if (write_block(inode->singleIndPtr, ind_buf) != 0) {
                free_data_block((unsigned int)new_block);
                return -1;
            }
        } else {
            check_block -= ptrs_per_block;
            unsigned int double_limit = ptrs_per_block * ptrs_per_block;
            if (check_block < double_limit) {
                if (inode->doubleIndPtr == 0) {
                    int ind1_block = alloc_data_block();
                    if (ind1_block < 0) {
                        free_data_block((unsigned int)new_block);
                        return -1;
                    }
                    inode->doubleIndPtr = (DWORD)ind1_block;
                }
                unsigned char *ind1_buf = (unsigned char *)__builtin_alloca(block_size);
                if (read_block(inode->doubleIndPtr, ind1_buf) != 0) {
                    free_data_block((unsigned int)new_block);
                    return -1;
                }
                DWORD *ptrs1 = (DWORD *)ind1_buf;
                
                unsigned int outer_idx = check_block / ptrs_per_block;
                unsigned int inner_idx = check_block % ptrs_per_block;
                
                if (ptrs1[outer_idx] == 0) {
                    int ind2_block = alloc_data_block();
                    if (ind2_block < 0) {
                        free_data_block((unsigned int)new_block);
                        return -1;
                    }
                    ptrs1[outer_idx] = (DWORD)ind2_block;
                    if (write_block(inode->doubleIndPtr, ind1_buf) != 0) {
                        free_data_block((unsigned int)ind2_block);
                        free_data_block((unsigned int)new_block);
                        return -1;
                    }
                }
                
                unsigned char *ind2_buf = (unsigned char *)__builtin_alloca(block_size);
                if (read_block(ptrs1[outer_idx], ind2_buf) != 0) {
                    free_data_block((unsigned int)new_block);
                    return -1;
                }
                DWORD *ptrs2 = (DWORD *)ind2_buf;
                ptrs2[inner_idx] = (DWORD)new_block;
                if (write_block(ptrs1[outer_idx], ind2_buf) != 0) {
                    free_data_block((unsigned int)new_block);
                    return -1;
                }
            } else {
                free_data_block((unsigned int)new_block);
                return -1;
            }
        }
    }

    inode->blocksFileSize++;
    if (write_inode(inode_num, inode) != 0) {
        free_data_block((unsigned int)new_block);
        return -1;
    }

    return new_block;
}

/* -------------------------------------------------------------------------
 * Auxiliar: lê o MBR e localiza a partição <partition>.
 * Preenche *first_sector e *num_sectors.
 * Retorna 0 em caso de sucesso.
 * ---------------------------------------------------------------------- */
static int read_partition_info(int partition,
                               unsigned int *first_sector,
                               unsigned int *num_sectors)
{
    unsigned char mbr_buf[SECTOR_SIZE];
    struct sofs_mbr *mbr;

    if (read_sector(0, mbr_buf) != 0)
        return -1;

    mbr = (struct sofs_mbr *)mbr_buf;

    if (partition < 0 || partition >= (int)mbr->numPartitions)
        return -1;

    *first_sector = mbr->partitionTable[partition].firstSector;
    *num_sectors  = mbr->partitionTable[partition].lastSector
                    - mbr->partitionTable[partition].firstSector + 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Funções básicas de criação/destruição de blocos de dados e i-nodes.
 *
 * Fornecidas como blocos de construção para a implementação do grupo em
 * sofs_create, sofs_delete, sofs_read, sofs_write, etc.
 * ---------------------------------------------------------------------- */

/*
 * alloc_data_block - aloca o primeiro bloco de dados livre.
 *
 * Pesquisa no bitmap de dados o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do bloco e retorna o número absoluto do bloco na partição.
 *
 * Retorna o número do bloco (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se o disco estiver cheio.
 */
static int alloc_data_block(void)
{
    int bit;
    unsigned int block_size;
    unsigned char *buf;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_DADOS, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_DADOS, bit, 1) != 0)
        return -1;

    /* Inicializa o bloco recém-alocado com zeros */
    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    buf = (unsigned char *)__builtin_alloca(block_size);
    memset(buf, 0, block_size);

    /* O primeiro bloco de dados começa após superbloco + bitmaps + área de i-nodes */
    unsigned int first_data_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;

    if (write_block(first_data_block + (unsigned int)bit, buf) != 0) {
        setBitmap2(BITMAP_DADOS, bit, 0);
        return -1;
    }

    return (int)(first_data_block + (unsigned int)bit);
}

/*
 * free_data_block - libera um bloco de dados previamente alocado.
 *
 *   abs_block_num : número absoluto do bloco na partição (conforme
 *                   retornado por alloc_data_block).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_data_block(unsigned int abs_block_num)
{
    unsigned int first_data_block;
    int bit;

    if (!g_mounted)
        return -1;

    first_data_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;

    if (abs_block_num < first_data_block)
        return -1;

    bit = (int)(abs_block_num - first_data_block);
    return setBitmap2(BITMAP_DADOS, bit, 0);
}

/*
 * alloc_inode - aloca o primeiro i-node livre.
 *
 * Pesquisa no bitmap de i-nodes o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do i-node em disco e retorna o número do i-node.
 *
 * Retorna o número do i-node (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se todos os i-nodes estiverem em uso.
 */
static int alloc_inode(void)
{
    int bit;
    unsigned int inode_block;
    unsigned int inodes_per_block;
    unsigned int inode_offset;
    unsigned char *buf;
    unsigned int block_size;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_INODE, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_INODE, bit, 1) != 0)
        return -1;

    /* Zera o i-node em disco */
    block_size     = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block    = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (unsigned int)bit / inodes_per_block;
    inode_offset   = (unsigned int)bit % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    memset(buf + inode_offset * sizeof(struct sofs_inode), 0,
           sizeof(struct sofs_inode));

    if (write_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    return bit;
}

/*
 * free_inode - libera um i-node previamente alocado.
 *
 *   inode_num : número do i-node (conforme retornado por alloc_inode).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_inode(unsigned int inode_num)
{
    if (!g_mounted)
        return -1;

    return setBitmap2(BITMAP_INODE, (int)inode_num, 0);
}

/* -------------------------------------------------------------------------
 * Gerência do sistema de arquivos
 * ---------------------------------------------------------------------- */

int sofs_identify(char *name, int size)
{
    const char *id = "Sistema Orgulhosamente Feito para Sisop (SOFS) - Felipe e Marcelo";
    if (name == NULL || size <= 0)
        return -1;
    strncpy(name, id, size - 1);
    name[size - 1] = '\0';
    return 0;
}

int sofs_format(int partition, int sectors_per_block)
{
    unsigned int first_sector, num_sectors;
    unsigned int num_blocks;
    unsigned int inode_area_blocks;
    unsigned int bitmap_blocks_data;
    unsigned int bitmap_blocks_inode;
    unsigned char block_buf[sectors_per_block * SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (sectors_per_block <= 0)
        return -1;

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Inicializa a camada de blocos para poder escrever na partição */
    if (init_block_layer(first_sector, (unsigned int)sectors_per_block) != 0)
        return -1;

    num_blocks = num_sectors / (unsigned int)sectors_per_block;

    /* 10% dos blocos para i-nodes, arredondado para cima */
    inode_area_blocks = (num_blocks + 9) / 10;

    /* Um bloco por 8*(sectors_per_block*SECTOR_SIZE) bits necessários em cada bitmap */
    bitmap_blocks_data  = (num_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);
    bitmap_blocks_inode = (inode_area_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);

    /* Constrói e grava o superbloco (bloco 0 da partição) */
    memset(block_buf, 0, sizeof(block_buf));
    sb = (struct sofs_superbloco *)block_buf;
    memcpy(sb->id, "SOFS", 4);
    sb->version              = 0x7E32;
    sb->superblockSize       = 1;
    sb->freeBlocksBitmapSize = (WORD)bitmap_blocks_data;
    sb->freeInodeBitmapSize  = (WORD)bitmap_blocks_inode;
    sb->inodeAreaSize        = (WORD)inode_area_blocks;
    sb->blockSize            = (WORD)sectors_per_block;
    sb->diskSize             = (DWORD)num_blocks;

    /* Checksum: complemento de um da soma dos 5 primeiros DWORDs */
    {
        DWORD *words = (DWORD *)block_buf;
        DWORD  sum   = words[0] + words[1] + words[2] + words[3] + words[4];
        sb->Checksum = ~sum;
    }

    if (write_block(0, block_buf) != 0)
        return -1;

    /* Inicializa com zeros as áreas de bitmap e de i-nodes */
    unsigned int block_size = (unsigned int)sectors_per_block * SECTOR_SIZE;
    unsigned char *zero_block = (unsigned char *)__builtin_alloca(block_size);
    memset(zero_block, 0, block_size);

    unsigned int total_bitmap_blocks = bitmap_blocks_data + bitmap_blocks_inode;
    for (unsigned int i = 1; i <= total_bitmap_blocks; i++) {
        if (write_block(i, zero_block) != 0)
            return -1;
    }

    unsigned int first_inode_block = 1 + total_bitmap_blocks;
    for (unsigned int i = 0; i < inode_area_blocks; i++) {
        if (write_block(first_inode_block + i, zero_block) != 0)
            return -1;
    }

    /* Inicializa o i-node 0 (Diretório Raiz) */
    struct sofs_inode root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.blocksFileSize = 1;
    root_inode.bytesFileSize = block_size;
    
    unsigned int first_data_block = 1 + total_bitmap_blocks + inode_area_blocks;
    root_inode.dataPtr[0] = first_data_block;
    root_inode.RefCounter = 1;

    if (write_block(first_data_block, zero_block) != 0)
        return -1;

    unsigned char *inode_block_buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(first_inode_block, inode_block_buf) != 0)
        return -1;
    memcpy(inode_block_buf, &root_inode, sizeof(struct sofs_inode));
    if (write_block(first_inode_block, inode_block_buf) != 0)
        return -1;

    /* Abre o bitmap para marcar o i-node 0 e o primeiro bloco de dados como ocupados */
    if (openBitmap2((int)first_sector) != 0)
        return -1;
    if (setBitmap2(BITMAP_INODE, 0, 1) != 0) {
        closeBitmap2();
        return -1;
    }
    if (setBitmap2(BITMAP_DADOS, 0, 1) != 0) {
        closeBitmap2();
        return -1;
    }
    if (closeBitmap2() != 0)
        return -1;

    return 0;
}

int sofs_mount(int partition)
{
    unsigned int first_sector, num_sectors;
    unsigned char sector_buf[SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (g_mounted)
        return -1;  /* partição já montada */

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Lê o primeiro setor da partição para obter o superbloco */
    if (read_sector(first_sector, sector_buf) != 0)
        return -1;

    sb = (struct sofs_superbloco *)sector_buf;

    /* Valida a assinatura do sistema de arquivos */
    if (memcmp(sb->id, "SOFS", 4) != 0)
        return -1;

    /* Agora sabemos o tamanho do bloco: inicializa a camada de blocos */
    if (init_block_layer(first_sector, (unsigned int)sb->blockSize) != 0)
        return -1;

    /* Abre o subsistema de bitmap */
    g_superbloco_sector = first_sector;
    if (openBitmap2((int)g_superbloco_sector) != 0)
        return -1;

    /* Armazena em cache o superbloco */
    memcpy(&g_superbloco, sb, sizeof(g_superbloco));
    g_mounted = true;
    return 0;
}

int sofs_umount(void)
{
    if (!g_mounted)
        return -1;

    closeBitmap2();
    reset_block_layer();
    memset(&g_superbloco, 0, sizeof(g_superbloco));
    g_mounted = false;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de arquivo (TODO)
 * ---------------------------------------------------------------------- */

SOFS_FILE sofs_create(char *filename)
{
    if (!g_mounted || filename == NULL || strlen(filename) == 0 || strlen(filename) > 50)
        return -1;

    unsigned int inode_num;
    BYTE type;
    
    /* Verifica se o arquivo já existe */
    if (find_file_inode(filename, &inode_num, &type, 0) == 0) {
        struct sofs_inode inode;
        if (read_inode(inode_num, &inode) != 0)
            return -1;

        /* Trunca o arquivo existente */
        truncate_file_blocks(&inode);
        if (write_inode(inode_num, &inode) != 0)
            return -1;

        /* Adiciona na tabela de arquivos abertos */
        int free_slot = -1;
        for (int i = 0; i < 10; i++) {
            if (!g_open_files[i].in_use) {
                free_slot = i;
                break;
            }
        }
        if (free_slot == -1)
            return -1;

        g_open_files[free_slot].in_use = true;
        g_open_files[free_slot].inode_num = inode_num;
        g_open_files[free_slot].curr_pos = 0;

        return (SOFS_FILE)free_slot;
    }

    /* O arquivo não existe, cria um novo */
    int new_inode = alloc_inode();
    if (new_inode < 0)
        return -1;

    struct sofs_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.RefCounter = 1;
    if (write_inode((unsigned int)new_inode, &inode) != 0) {
        free_inode((unsigned int)new_inode);
        return -1;
    }

    /* Insere no diretório raiz */
    if (find_or_allocate_directory_slot(filename, (unsigned int)new_inode) < 0) {
        free_inode((unsigned int)new_inode);
        return -1;
    }

    /* Adiciona na tabela de arquivos abertos */
    int free_slot = -1;
    for (int i = 0; i < 10; i++) {
        if (!g_open_files[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1)
        return -1;

    g_open_files[free_slot].in_use = true;
    g_open_files[free_slot].inode_num = (unsigned int)new_inode;
    g_open_files[free_slot].curr_pos = 0;

    return (SOFS_FILE)free_slot;
}

int sofs_delete(char *name)
{
    if (!g_mounted || name == NULL)
        return -1;

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    int found_idx = -1;
    struct sofs_record rec;

    for (unsigned int idx = 0; idx < total_records; idx++) {
        if (read_directory_record(idx, &rec) == 0 && rec.TypeVal != TYPEVAL_INVALIDO) {
            if (strcmp(rec.name, name) == 0) {
                found_idx = (int)idx;
                break;
            }
        }
    }

    if (found_idx == -1)
        return -1;

    struct sofs_inode file_inode;
    if (read_inode(rec.inodeNumber, &file_inode) != 0)
        return -1;

    if (file_inode.RefCounter > 1) {
        file_inode.RefCounter--;
        if (write_inode(rec.inodeNumber, &file_inode) != 0)
            return -1;
    } else {
        truncate_file_blocks(&file_inode);
        if (free_inode(rec.inodeNumber) != 0)
            return -1;
    }

    rec.TypeVal = TYPEVAL_INVALIDO;
    memset(rec.name, 0, sizeof(rec.name));
    rec.inodeNumber = 0;
    if (write_directory_record((unsigned int)found_idx, &rec) != 0)
        return -1;

    return 0;
}

SOFS_FILE sofs_open(char *name)
{
    if (!g_mounted || name == NULL)
        return -1;

    unsigned int inode_num;
    BYTE type;
    if (find_file_inode(name, &inode_num, &type, 0) != 0)
        return -1;

    int free_slot = -1;
    for (int i = 0; i < 10; i++) {
        if (!g_open_files[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1)
        return -1;

    g_open_files[free_slot].in_use = true;
    g_open_files[free_slot].inode_num = inode_num;
    g_open_files[free_slot].curr_pos = 0;

    return (SOFS_FILE)free_slot;
}

int sofs_close(SOFS_FILE handle)
{
    if (!g_mounted || !is_valid_handle(handle))
        return -1;
    g_open_files[handle].in_use = false;
    return 0;
}

int sofs_read(SOFS_FILE handle, char *buffer, int size)
{
    if (!g_mounted || !is_valid_handle(handle) || buffer == NULL || size < 0)
        return -1;

    if (size == 0)
        return 0;

    unsigned int inode_num = g_open_files[handle].inode_num;
    struct sofs_inode inode;
    if (read_inode(inode_num, &inode) != 0)
        return -1;

    unsigned int curr_pos = g_open_files[handle].curr_pos;
    if (curr_pos >= inode.bytesFileSize)
        return 0;

    unsigned int bytes_to_read = size;
    if (curr_pos + bytes_to_read > inode.bytesFileSize) {
        bytes_to_read = inode.bytesFileSize - curr_pos;
    }

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char *temp_buf = (unsigned char *)__builtin_alloca(block_size);
    unsigned int bytes_read = 0;

    while (bytes_read < bytes_to_read) {
        unsigned int file_block = (curr_pos + bytes_read) / block_size;
        unsigned int block_offset = (curr_pos + bytes_read) % block_size;
        unsigned int bytes_this_step = bytes_to_read - bytes_read;
        if (bytes_this_step > block_size - block_offset) {
            bytes_this_step = block_size - block_offset;
        }

        int phys_block = get_physical_block(&inode, file_block);
        if (phys_block < 0)
            return -1;

        if (phys_block > 0) {
            if (read_block((unsigned int)phys_block, temp_buf) != 0)
                return -1;
        } else {
            memset(temp_buf, 0, block_size);
        }

        memcpy(buffer + bytes_read, temp_buf + block_offset, bytes_this_step);
        bytes_read += bytes_this_step;
    }

    g_open_files[handle].curr_pos += bytes_read;
    return (int)bytes_read;
}

int sofs_write(SOFS_FILE handle, char *buffer, int size)
{
    if (!g_mounted || !is_valid_handle(handle) || buffer == NULL || size < 0)
        return -1;

    if (size == 0)
        return 0;

    unsigned int inode_num = g_open_files[handle].inode_num;
    struct sofs_inode inode;
    if (read_inode(inode_num, &inode) != 0)
        return -1;

    unsigned int curr_pos = g_open_files[handle].curr_pos;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char *temp_buf = (unsigned char *)__builtin_alloca(block_size);
    unsigned int bytes_written = 0;

    while (bytes_written < (unsigned int)size) {
        unsigned int file_block = (curr_pos + bytes_written) / block_size;
        unsigned int block_offset = (curr_pos + bytes_written) % block_size;
        unsigned int bytes_this_step = size - bytes_written;
        if (bytes_this_step > block_size - block_offset) {
            bytes_this_step = block_size - block_offset;
        }

        int phys_block = allocate_file_block(&inode, inode_num, file_block);
        if (phys_block < 0) {
            if (bytes_written == 0)
                return -1;
            break;
        }

        if (block_offset > 0 || bytes_this_step < block_size) {
            if (read_block((unsigned int)phys_block, temp_buf) != 0)
                return -1;
        }

        memcpy(temp_buf + block_offset, buffer + bytes_written, bytes_this_step);

        if (write_block((unsigned int)phys_block, temp_buf) != 0)
            return -1;

        bytes_written += bytes_this_step;
    }

    unsigned int new_pos = curr_pos + bytes_written;
    if (new_pos > inode.bytesFileSize) {
        inode.bytesFileSize = new_pos;
        if (write_inode(inode_num, &inode) != 0)
            return -1;
    }

    g_open_files[handle].curr_pos = new_pos;
    return (int)bytes_written;
}

/* -------------------------------------------------------------------------
 * Operações de diretório (TODO)
 * ---------------------------------------------------------------------- */

int sofs_opendir(void)
{
    if (!g_mounted)
        return -1;

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    g_dir_open = true;
    g_dir_curr_record = total_records;

    for (unsigned int idx = 0; idx < total_records; idx++) {
        struct sofs_record rec;
        if (read_directory_record(idx, &rec) == 0) {
            if (rec.TypeVal != TYPEVAL_INVALIDO) {
                g_dir_curr_record = idx;
                break;
            }
        }
    }
    return 0;
}

int sofs_readdir(SOFS_DIRENT *dentry)
{
    if (!g_mounted || !g_dir_open || dentry == NULL)
        return -1;

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    if (g_dir_curr_record >= total_records)
        return -1;

    struct sofs_record rec;
    if (read_directory_record(g_dir_curr_record, &rec) != 0)
        return -1;

    strncpy(dentry->name, rec.name, SOFS_MAX_FILE_NAME_SIZE);
    dentry->name[SOFS_MAX_FILE_NAME_SIZE] = '\0';
    dentry->fileType = rec.TypeVal;

    struct sofs_inode file_inode;
    if (read_inode(rec.inodeNumber, &file_inode) != 0) {
        dentry->fileSize = 0;
    } else {
        dentry->fileSize = file_inode.bytesFileSize;
    }

    unsigned int next_idx = g_dir_curr_record + 1;
    g_dir_curr_record = total_records;
    for (unsigned int idx = next_idx; idx < total_records; idx++) {
        struct sofs_record next_rec;
        if (read_directory_record(idx, &next_rec) == 0) {
            if (next_rec.TypeVal != TYPEVAL_INVALIDO) {
                g_dir_curr_record = idx;
                break;
            }
        }
    }

    return 0;
}

int sofs_closedir(void)
{
    if (!g_mounted || !g_dir_open)
        return -1;
    g_dir_open = false;
    g_dir_curr_record = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de link (TODO)
 * ---------------------------------------------------------------------- */

int sofs_sln(char *linkname, char *filename)
{
    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;
    if (strlen(linkname) == 0 || strlen(linkname) > 50)
        return -1;
    if (strlen(filename) == 0 || strlen(filename) > 50)
        return -1;

    unsigned int existing_inode;
    if (find_file_inode(linkname, &existing_inode, NULL, 0) == 0)
        return -1;

    int link_inode_num = alloc_inode();
    if (link_inode_num < 0)
        return -1;

    int data_block_num = alloc_data_block();
    if (data_block_num < 0) {
        free_inode((unsigned int)link_inode_num);
        return -1;
    }

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char *buf = (unsigned char *)__builtin_alloca(block_size);
    memset(buf, 0, block_size);
    strncpy((char *)buf, filename, block_size - 1);
    buf[block_size - 1] = '\0';
    
    if (write_block((unsigned int)data_block_num, buf) != 0) {
        free_data_block((unsigned int)data_block_num);
        free_inode((unsigned int)link_inode_num);
        return -1;
    }

    struct sofs_inode link_inode;
    memset(&link_inode, 0, sizeof(link_inode));
    link_inode.blocksFileSize = 1;
    link_inode.bytesFileSize = (DWORD)(strlen(filename) + 1);
    link_inode.dataPtr[0] = (DWORD)data_block_num;
    link_inode.RefCounter = 1;

    if (write_inode((unsigned int)link_inode_num, &link_inode) != 0) {
        free_data_block((unsigned int)data_block_num);
        free_inode((unsigned int)link_inode_num);
        return -1;
    }

    if (find_or_allocate_directory_slot(linkname, (unsigned int)link_inode_num) < 0) {
        free_data_block((unsigned int)data_block_num);
        free_inode((unsigned int)link_inode_num);
        return -1;
    }

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;
    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    for (unsigned int idx = 0; idx < total_records; idx++) {
        struct sofs_record rec;
        if (read_directory_record(idx, &rec) == 0 && rec.TypeVal != TYPEVAL_INVALIDO) {
            if (strcmp(rec.name, linkname) == 0) {
                rec.TypeVal = TYPEVAL_LINK;
                if (write_directory_record(idx, &rec) != 0)
                    return -1;
                break;
            }
        }
    }

    return 0;
}

int sofs_hln(char *linkname, char *filename)
{
    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;
    if (strlen(linkname) == 0 || strlen(linkname) > 50)
        return -1;

    unsigned int target_inode_num;
    BYTE target_type;
    if (find_file_inode(filename, &target_inode_num, &target_type, 0) != 0)
        return -1;

    unsigned int existing_inode;
    if (find_file_inode(linkname, &existing_inode, NULL, 0) == 0)
        return -1;

    struct sofs_inode target_inode;
    if (read_inode(target_inode_num, &target_inode) != 0)
        return -1;
    target_inode.RefCounter++;
    if (write_inode(target_inode_num, &target_inode) != 0)
        return -1;

    if (find_or_allocate_directory_slot(linkname, target_inode_num) < 0) {
        target_inode.RefCounter--;
        write_inode(target_inode_num, &target_inode);
        return -1;
    }

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;
    unsigned int total_records = root_inode.bytesFileSize / sizeof(struct sofs_record);
    for (unsigned int idx = 0; idx < total_records; idx++) {
        struct sofs_record rec;
        if (read_directory_record(idx, &rec) == 0 && rec.TypeVal != TYPEVAL_INVALIDO) {
            if (strcmp(rec.name, linkname) == 0) {
                rec.TypeVal = target_type;
                if (write_directory_record(idx, &rec) != 0)
                    return -1;
                break;
            }
        }
    }

    return 0;
}
