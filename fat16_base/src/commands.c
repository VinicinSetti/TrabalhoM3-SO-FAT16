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


void mv(FILE *fp, char *filename, struct fat_bpb *bpb){
    ;; /* TODO */
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

