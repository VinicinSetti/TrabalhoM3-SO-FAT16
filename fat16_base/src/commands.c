#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "commands.h"
#include "fat16.h"
#include "support.h"

off_t fsize(const char *filename){
    struct stat st;
    if (stat(filename, &st) == 0)
        return st.st_size;
    return -1;
}

struct fat_dir find(struct fat_dir *dirs, char *filename, struct fat_bpb *bpb){
    struct fat_dir curdir;
    int dirs_len = sizeof(struct fat_dir) * bpb->possible_rentries;
    int i;

    for (i=0; i < dirs_len; i++){
        if (strcmp((char *) dirs[i].name, filename) == 0){
            curdir = dirs[i];
            break;
        }
    }
    return curdir;
}

// Função para encontrar um cluster livre no FAT16
uint32_t find_empty_cluster(FILE *fp, struct fat_bpb *bpb) {
    uint32_t fat_offset = bpb->bytes_p_sect * bpb->reserved_sect;
    uint32_t fat_entries = bpb->sect_per_fat * bpb->bytes_p_sect / 2;
    uint16_t fat_val;

    for (uint32_t i = 0; i < fat_entries; i++) {
        fseek(fp, fat_offset + i * 2, SEEK_SET);
        fread(&fat_val, 2, 1, fp);

        if (fat_val == 0x0000) {
            return bpb->reserved_sect + bpb->sect_per_fat + i;
        }
    }

    return -1; // Nenhum cluster livre encontrado
}

struct fat_dir *ls(FILE *fp, struct fat_bpb *bpb){
    int i;
    struct fat_dir *dirs = malloc(sizeof (struct fat_dir) * bpb->possible_rentries);

    for (i=0; i < bpb->possible_rentries; i++){
        uint32_t offset = bpb_froot_addr(bpb) + i * 32;
        read_bytes(fp, offset, &dirs[i], sizeof(dirs[i]));
    }
    return dirs;
}

int write_dir(FILE *fp, char *fname, struct fat_dir *dir){
    char* name = padding(fname);
    strcpy((char *) dir->name, (char *) name);
    if (fwrite(dir, 1, sizeof(struct fat_dir), fp) <= 0)
        return -1;
    return 0;
}

int write_data(FILE *fp, char *fname, struct fat_dir *dir, struct fat_bpb *bpb){

    FILE *localf = fopen(fname, "r");
    int c;

    while ((c = fgetc(localf)) != EOF){
        if (fputc(c, fp) != c)
            return -1;
    }
    return 0;
}

int wipe(FILE *fp, struct fat_dir *dir, struct fat_bpb *bpb){
    int start_offset = bpb_froot_addr(bpb) + (bpb->bytes_p_sect * \
            dir->starting_cluster);
    int limit_offset = start_offset + dir->file_size;

    while (start_offset <= limit_offset){
        fseek(fp, ++start_offset, SEEK_SET);
        if(fputc(0x0, fp) != 0x0)
            return 01;
    }
    return 0;
}

void mv(FILE *fp, char *filename, struct fat_bpb *bpb) {
    // Encontrar o diretório do arquivo a ser movido
    struct fat_dir *dirs = ls(fp, bpb);
    struct fat_dir file_to_move = find(dirs, filename, bpb);

    // Verificar se o arquivo existe/
    if (strncmp((char *) file_to_move.name, filename, 11) != 0) {
        fprintf(stderr, "Arquivo não encontrado\n");
        free(dirs);  // Liberar memória antes de retornar
        return;
    }

    // Marcar o diretório do arquivo como excluído
    if (wipe(fp, &file_to_move, bpb) != 0) {
        fprintf(stderr, "Erro ao limpar os clusters do arquivo\n");
        free(dirs);  // Liberar memória antes de retornar
        return;
    }

    // Encontrar o índice do diretório para marcar como excluído
    int dir_index = -1;
    for (int i = 0; i < bpb->possible_rentries; i++) {
        if (memcmp(&dirs[i], &file_to_move, sizeof(struct fat_dir)) == 0) {
            dir_index = i;
            break;
        }
    }

    cp(fp, filename, filename, bpb);

    if (dir_index != -1) {
        uint32_t dir_offset = bpb_froot_addr(bpb) + dir_index * sizeof(struct fat_dir);
        fseek(fp, dir_offset, SEEK_SET);
        if (fputc(DIR_FREE_ENTRY, fp) == EOF) {
            fprintf(stderr, "Erro ao marcar o diretório como excluído\n");
        } else {
            printf("Arquivo '%s' movido com sucesso\n", filename);
        }
    } else {
        fprintf(stderr, "Erro ao encontrar o índice do diretório\n");
    }

    // Liberar memória alocada
    free(dirs);
}

void update_fat(FILE *fp, uint32_t current_cluster, uint32_t next_cluster, struct fat_bpb *bpb) {
    uint32_t fat_offset = bpb_faddress(bpb) + current_cluster * 2;  // FAT16 usa 2 bytes por entrada
    fseek(fp, fat_offset, SEEK_SET);
    fwrite(&next_cluster, 2, 1, fp);
}


void mv2(FILE *fp, const char *filename, struct fat_bpb *bpb) {
    fprintf(stdout, "teste");
    // Abrir o arquivo externo
    FILE *src_file = fopen(filename, "rb");
    if (src_file == NULL) {
        fprintf(stderr, "Erro ao abrir o arquivo externo '%s'\n", filename);
        return;
    }

    // Obter o tamanho do arquivo
    off_t file_size = fsize(filename);
    if (file_size == -1) {
        fprintf(stderr, "Erro ao obter o tamanho do arquivo '%s'\n", filename);
        fclose(src_file);
        return;
    }

    // Encontrar o primeiro cluster livre
    uint32_t first_cluster = find_empty_cluster(fp, bpb);
    if (first_cluster == (uint32_t)-1) {
        fprintf(stderr, "Erro ao encontrar um cluster livre\n");
        fclose(src_file);
        return;
    }

    // Preparar a entrada de diretório
    struct fat_dir new_entry = {0};
    char *name_padded = padding(filename);
    strncpy((char *)new_entry.name, name_padded, 11);
    new_entry.attr = 0;
    new_entry.starting_cluster = first_cluster;
    new_entry.file_size = file_size;

    // Escrever os dados do arquivo nos clusters
    uint32_t cluster_size = bpb->bytes_p_sect * bpb->sector_p_clust;
    uint32_t remaining_size = file_size;
    uint32_t current_cluster = first_cluster;
    uint8_t *buffer = malloc(cluster_size);

    if (buffer == NULL) {
        fprintf(stderr, "Erro ao alocar memória para buffer\n");
        fclose(src_file);
        return;
    }

    while (remaining_size > 0) {
        // Ler os dados do arquivo externo
        size_t bytes_to_write = remaining_size > cluster_size ? cluster_size : remaining_size;
        fread(buffer, 1, bytes_to_write, src_file);

        // Calcular o offset do cluster atual e escrever os dados
        uint32_t cluster_offset = bpb_clust_addr(bpb, current_cluster);
        fseek(fp, cluster_offset, SEEK_SET);
        fwrite(buffer, 1, bytes_to_write, fp);

        remaining_size -= bytes_to_write;
        if (remaining_size > 0) {
            // Encontrar o próximo cluster livre
            uint32_t next_cluster = find_empty_cluster(fp, bpb);
            if (next_cluster == (uint32_t)-1) {
                fprintf(stderr, "Erro ao encontrar o próximo cluster livre\n");
                free(buffer);
                fclose(src_file);
                return;
            }

            // Atualizar a tabela FAT para vincular os clusters
            update_fat(fp, current_cluster, next_cluster, bpb);
            current_cluster = next_cluster;
        } else {
            // Marcar o fim do arquivo na tabela FAT
            update_fat(fp, current_cluster, 0xFFFF, bpb);  // 0xFFFF indica fim de arquivo no FAT16
        }
    }

    free(buffer);
    fclose(src_file);

    // Escrever a nova entrada de diretório no diretório raiz
    struct fat_dir *dirs = ls(fp, bpb);
    for (int i = 0; i < bpb->possible_rentries; i++) {
        if (dirs[i].name[0] == DIR_FREE_ENTRY || dirs[i].name[0] == 0x00) {
            uint32_t dir_offset = bpb_froot_addr(bpb) + i * sizeof(struct fat_dir);
            fseek(fp, dir_offset, SEEK_SET);
            fwrite(&new_entry, sizeof(struct fat_dir), 1, fp);
            printf("Arquivo '%s' adicionado com sucesso\n", filename);
            free(dirs);

            if (remove(filename) != 0) {
                fprintf(stderr, "Erro ao deletar o arquivo de origem '%s'\n", filename);
            } else {
                printf("Arquivo de origem '%s' deletado com sucesso\n", filename);
            }
            return;
        }
        
    }

    fprintf(stderr, "Erro ao encontrar um slot livre no diretório raiz\n");
    free(dirs);
}

void rm(FILE *fp, char *filename, struct fat_bpb *bpb){

    struct fat_dir *dirs = ls(fp, bpb);
    struct fat_dir dir_to_remove = find(dirs, filename, bpb);

    if (strncmp((char *) dir_to_remove.name, filename, 11) == 0) {
        if (wipe(fp, &dir_to_remove, bpb) != 0) {
            fprintf(stderr, "Erro ao limpar os clusters do arquivo\n");
            free(dirs);
            return;
        }

        int dir_index = -1;
        for (int i = 0; i < bpb->possible_rentries; i++) {
            if (memcmp(&dirs[i], &dir_to_remove, sizeof(struct fat_dir)) == 0) {
                dir_index = i;
                break;
            }
        }

        if (dir_index != -1) {

            uint32_t dir_offset = bpb_froot_addr(bpb) + dir_index * sizeof(struct fat_dir);

            fseek(fp, dir_offset, SEEK_SET);
            fputc(0xE5, fp);
        } else {
            fprintf(stderr, "Erro ao encontrar o índice do diretório\n");
        }
    } else {
        fprintf(stderr, "Arquivo não encontrado\n");
    }

    free(dirs);
}


void cp(FILE *fp, char *filename, char *file_dst_name, struct fat_bpb *bpb){

    struct fat_dir *dir = ls(fp, bpb);
    struct fat_dir file_dir = find(dir, filename, bpb);

    FILE *dst_file = fopen(file_dst_name, "w");
    if (!dst_file) {
        perror("Erro ao abrir arquivo destino");
        return;
    }

    uint32_t cluster = file_dir.starting_cluster;
    uint32_t cluster_size = bpb->bytes_p_sect * bpb->sector_p_clust;
    uint32_t file_size = file_dir.file_size;
    uint32_t bytes_to_read;

    char *buffer = malloc(cluster_size);
    if (!buffer) {
        perror("Erro ao alocar buffer");
        fclose(dst_file);
        free(dir);
        return;
    }

    while (file_size > 0) {
        bytes_to_read = file_size > cluster_size ? cluster_size : file_size;
        uint32_t offset = bpb_clust_addr(bpb, cluster);
        fseek(fp, offset, SEEK_SET);
        fread(buffer, 1, bytes_to_read, fp);
        fwrite(buffer, 1, bytes_to_read, dst_file);

        file_size -= bytes_to_read;
        cluster = fat_next_cluster(fp, bpb, cluster);
    }

    free(buffer);
    fclose(dst_file);
    free(dir);

}

