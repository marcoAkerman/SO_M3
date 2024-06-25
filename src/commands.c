#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "commands.h"
#include "fat16.h"
#include "support.h"

// off_t fsize(const char *filename){
//     struct stat st;
//     if (stat(filename, &st) == 0)
//         return st.st_size;
//     return -1;
// }

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

int wipe2(FILE *fp, struct fat_dir *dir, struct fat_bpb *bpb) {
    // Calcula o endereço inicial da área de dados
    uint32_t data_start = bpb_fdata_addr(bpb);
    // Calcula o offset inicial do cluster do arquivo
    uint32_t start_offset = data_start + ((dir->starting_cluster - 2) * bpb->sector_p_clust * bpb->bytes_p_sect);
    uint32_t limit_offset = start_offset + dir->file_size;

    while (start_offset <= limit_offset) {
        fseek(fp, start_offset, SEEK_SET); // Use start_offset diretamente sem incrementar primeiro
        printf("Writing at offset: %u\n", start_offset); // Imprime o offset atual
        if (fputc(0x0, fp) == EOF) // Escreve 0xAA em vez de 0x0
            return 1;
        start_offset++; // Incrementa start_offset após escrever
    }
    return 0;
}

uint16_t get_next_cluster(FILE *fp, struct fat_bpb *bpb, uint16_t cluster) {
    // Calcula o endereço da tabela FAT
    uint32_t fat_addr = bpb_faddress(bpb);
    // Cada entrada FAT16 tem 2 bytes
    uint32_t offset = fat_addr + (cluster * 2);
    uint16_t next_cluster;

    // Move para o offset da entrada FAT correspondente e lê o próximo cluster
    fseek(fp, offset, SEEK_SET);
    fread(&next_cluster, sizeof(uint16_t), 1, fp);

    return next_cluster;
}




uint16_t allocate_free_cluster(FILE *fp, struct fat_bpb *bpb) {
    // Calcula o endereço da tabela FAT
    uint32_t fat_addr = bpb_faddress(bpb);
    uint16_t total_clusters = (bpb->large_n_sects - bpb->reserved_sect - (bpb->n_fat * bpb->sect_per_fat) - (bpb->possible_rentries * 32 / bpb->bytes_p_sect)) / bpb->sector_p_clust;
    uint16_t cluster;

    // Percorre a tabela FAT para encontrar um cluster livre
    for (cluster = 2; cluster < total_clusters; cluster++) {
        uint32_t offset = fat_addr + (cluster * 2);
        uint16_t entry;
        if (read_bytes(fp, offset, &entry, sizeof(entry)) != 0) {
            perror("Erro ao ler a tabela FAT");
            return 0xFFFF; // Indica erro
        }
        if (entry == 0x0000) {
            // Encontrado cluster livre
            uint16_t end_of_chain = 0xFFFF;
            if (fseek(fp, offset, SEEK_SET) != 0 || fwrite(&end_of_chain, sizeof(end_of_chain), 1, fp) != 1) {
                perror("Erro ao escrever na tabela FAT");
                return 0xFFFF; // Indica erro
            }
            return cluster;
        }
    }

    return 0xFFFF; // Indica que não há clusters livres
}




int write_dir2(FILE *fp, char *fname, struct fat_dir *dir, struct fat_bpb *bpb) {
    // Calcular o endereço inicial do diretório raiz
    uint32_t root_dir_start = bpb_froot_addr(bpb);
    uint32_t root_dir_size = bpb->possible_rentries * sizeof(struct fat_dir);
    uint32_t offset;

    // Procurar uma entrada livre no diretório raiz
    struct fat_dir temp_dir;
    for (offset = 0; offset < root_dir_size; offset += sizeof(struct fat_dir)) {
        if (read_bytes(fp, root_dir_start + offset, &temp_dir, sizeof(struct fat_dir)) != 0) {
            perror("Erro ao ler o diretório raiz");
            return -1;
        }
        if (temp_dir.name[0] == DIR_FREE_ENTRY || temp_dir.name[0] == 0x00) {
            // Encontrada uma entrada livre, escrever a nova entrada aqui
            if (fseek(fp, root_dir_start + offset, SEEK_SET) != 0 || fwrite(dir, sizeof(struct fat_dir), 1, fp) != 1) {
                perror("Erro ao escrever a entrada do diretório");
                return -1;
            }
            return 0;
        }
    }

    // Se não houver entradas livres
    return -1;
}

int write_data2(FILE *fp, FILE *localf, struct fat_dir *dir, struct fat_bpb *bpb) {
    uint32_t data_start = bpb_fdata_addr(bpb);
    uint32_t cluster_size = bpb->sector_p_clust * bpb->bytes_p_sect;
    uint32_t bytes_remaining = dir->file_size;
    uint16_t current_cluster = dir->starting_cluster;

    unsigned char *buffer = (unsigned char *)malloc(cluster_size);
    if (buffer == NULL) {
        perror("Erro ao alocar memória");
        return -1;
    }

    while (bytes_remaining > 0) {
        uint32_t bytes_to_write = bytes_remaining < cluster_size ? bytes_remaining : cluster_size;

        if (fread(buffer, 1, bytes_to_write, localf) != bytes_to_write) {
            perror("Erro ao ler dados do arquivo local");
            free(buffer);
            return -1;
        }

        uint32_t current_offset = data_start + ((current_cluster - 2) * cluster_size);
        if (fseek(fp, current_offset, SEEK_SET) != 0 || fwrite(buffer, 1, bytes_to_write, fp) != bytes_to_write) {
            perror("Erro ao escrever dados para o FAT16");
            free(buffer);
            return -1;
        }

        bytes_remaining -= bytes_to_write;

        if (bytes_remaining > 0) {
            uint16_t next_cluster = allocate_free_cluster(fp, bpb);
            if (next_cluster == 0xFFFF) {
                fprintf(stderr, "Erro ao alocar próximo cluster");
                free(buffer);
                return -1;
            }

            // Atualiza a tabela FAT para apontar para o próximo cluster
            uint32_t fat_offset = bpb_faddress(bpb) + (current_cluster * 2);
            if (fseek(fp, fat_offset, SEEK_SET) != 0 || fwrite(&next_cluster, sizeof(next_cluster), 1, fp) != 1) {
                perror("Erro ao atualizar a tabela FAT");
                free(buffer);
                return -1;
            }

            current_cluster = next_cluster;
        }
    }

    free(buffer);
    return 0;
}


void rm(FILE *fp, char *filename, struct fat_bpb *bpb){
    struct fat_dir *dirs = ls(fp, bpb);
    struct fat_dir dir = find(dirs, padding(filename), bpb);
    if (dir.starting_cluster == 0) {
        free(dirs);
        return; // File not found
    }

    // Wipe the file data
    wipe2(fp, &dir, bpb);
    // Mark the directory entry as deleted
    dir.name[0] = 0xE5;

    // Write the updated directory entry back to the disk
    int dir_index = -1;
    for (int i = 0; i < bpb->possible_rentries; i++) {
        if (strcmp((char *)dirs[i].name, padding(filename)) == 0) {
            dir_index = i;
            break;
        }
    }

    // Write the updated directory entry back to the disk
    int offset = bpb_froot_addr(bpb) + (dir_index * sizeof(struct fat_dir));
    fseek(fp, offset, SEEK_SET);
    fwrite(&dir, sizeof(struct fat_dir), 1, fp);

    free(dirs);
}


void cp(FILE *fp, char *filename, struct fat_bpb *bpb){ // copia do fat16 para um arquivo local
    struct fat_dir *dirs = ls(fp, bpb);
    struct fat_dir file = find(dirs, padding(filename), bpb);

    FILE *local_file = fopen("copia.txt", "w");
    if (local_file == NULL) {
        perror("Erro ao abrir o arquivo local");
        return;
    }

    // Calcular o endereço inicial da área de dados
    uint32_t data_start = bpb_fdata_addr(bpb);
    // Calcular o tamanho de um cluster em bytes
    uint32_t cluster_size = bpb->sector_p_clust * bpb->bytes_p_sect;

    // Lê e escreve os dados do arquivo FAT16 para o arquivo local byte a byte
    uint32_t bytes_remaining = file.file_size;
    uint32_t current_cluster = file.starting_cluster;

    while (bytes_remaining > 0) {
        // Calcula o offset do cluster atual
        uint32_t current_offset = data_start + ((current_cluster - 2) * cluster_size);
        
        // Move para o offset do cluster atual
        fseek(fp, current_offset, SEEK_SET);

        // Lê e escreve os dados byte a byte
        for (uint32_t i = 0; i < cluster_size && bytes_remaining > 0; i++, bytes_remaining--) {
            int byte = fgetc(fp);
            if (byte == EOF) {
                perror("Erro ao ler dados do arquivo FAT16");
                fclose(local_file);
                return;
            }

            if (fputc(byte, local_file) == EOF) {
                perror("Erro ao escrever dados no arquivo local");
                fclose(local_file);
                return;
            }
        }

        // Atualiza o current_cluster para o próximo cluster na cadeia
        current_cluster = get_next_cluster(fp, bpb, current_cluster);
        if (current_cluster >= 0xFFF8) { // 0xFFF8 indica o fim da cadeia de clusters
            break;
        }
    }

    // Fecha o arquivo local
    fclose(local_file);
}


void mv(FILE *fp, char *filename, struct fat_bpb *bpb){ // copia um arquivo local para o fat 16
    FILE *local_file = fopen(filename, "r");
    if (local_file == NULL) {
        perror("Erro ao abrir o arquivo local");
        return;
    }

    // Cria uma nova entrada de diretório
    struct fat_dir dir;
    memset(&dir, 0, sizeof(dir));

    // Preencher a estrutura do diretório
    char *padded_name = padding(filename);
    strncpy((char *)dir.name, padded_name, 11);
    dir.attr = 0; // Definir atributos apropriados
    dir.starting_cluster = allocate_free_cluster(fp, bpb);

    // Obter o tamanho do arquivo local
    fseek(local_file, 0, SEEK_END);
    dir.file_size = ftell(local_file);
    fseek(local_file, 0, SEEK_SET);

    // Escrever os dados do arquivo local para o FAT16
    if (write_data2(fp, local_file, &dir, bpb) != 0) {
        fprintf(stderr, "Erro ao escrever dados para o FAT16\n");
        fclose(local_file);
        return;
    }

    // Escrever a entrada do diretório no FAT16
    if (write_dir2(fp, filename, &dir, bpb) != 0) {
        fprintf(stderr, "Erro ao escrever a entrada do diretório no FAT16\n");
        fclose(local_file);
        return;
    }

    // Fecha o arquivo local
    fclose(local_file);
}

