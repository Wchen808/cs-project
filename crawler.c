#define _POSIX_C_SOURCE 200809L
#include <stdio.h>      
#include <stdlib.h>    
#include <pthread.h>   
#include <string.h>    
#include <unistd.h>    
#include <curl/curl.h>  

// Node = one Wikipedia page in the BFS graph
typedef struct Node {
    char *url;              
    int depth;              
    struct Node *parent;    
    struct Node *next;     
} Node;

// Thread-safe queue of Nodes (FIFO)
typedef struct {
    Node *head;
    Node *tail;
    pthread_mutex_t lock;   
} URLQueue;

typedef struct VisitedNode {
    char *url;
    struct VisitedNode *next;
} VisitedNode;



static URLQueue url_queue;
static VisitedNode *visited_head = NULL;

static pthread_mutex_t visited_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t result_lock  = PTHREAD_MUTEX_INITIALIZER;

static char *target_url = NULL;  
static char *start_url  = NULL;  
static int   max_depth  = 0;     
static int   num_threads = 8;   

static int   found = 0;          
static Node *found_node = NULL;  

static const char *WIKI_PREFIX = "https://en.wikipedia.org/wiki/";

// Initialize an empty queue
void init_queue(URLQueue *q) {
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->lock, NULL);
}

// Add a node to the tail of the queue
void enqueue_node(URLQueue *q, Node *node) {
    node->next = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    pthread_mutex_unlock(&q->lock);
}

// Remove and return a node from the head of the queue
Node *dequeue_node(URLQueue *q) {
    pthread_mutex_lock(&q->lock);
    Node *node = q->head;
    if (node) {
        q->head = node->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
    }
    pthread_mutex_unlock(&q->lock);
    return node;
}


// Check if URL is already in visited list (visited_lock must be held)
int visited_contains_locked(const char *url) {
    VisitedNode *cur = visited_head;
    while (cur) {
        if (strcmp(cur->url, url) == 0) {
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

// Add URL to visited list (visited_lock must be held)
void visited_add_locked(const char *url) {
    VisitedNode *node = malloc(sizeof(VisitedNode));
    if (!node) {
        perror("malloc visited");
        exit(EXIT_FAILURE);
    }
    node->url = strdup(url);
    if (!node->url) {
        perror("strdup visited");
        exit(EXIT_FAILURE);
    }
    node->next = visited_head;
    visited_head = node;
}


// Create a new Node (page) with given URL/depth/parent
Node *create_node(const char *url, int depth, Node *parent) {
    Node *node = malloc(sizeof(Node));
    if (!node) {
        perror("malloc node");
        exit(EXIT_FAILURE);
    }
    node->url = strdup(url);
    if (!node->url) {
        perror("strdup node->url");
        exit(EXIT_FAILURE);
    }
    node->depth = depth;
    node->parent = parent;
    node->next = NULL;
    return node;
}
void normalize_inplace(char *url) {
    char *hash = strchr(url, '#');
    if (hash) *hash = '\0';

    char *q = strchr(url, '?');
    if (q) *q = '\0';

    size_t len = strlen(url);
    size_t min_len = strlen("https://en.wikipedia.org");
    while (len > min_len && len > 0 && url[len - 1] == '/') {
        url[len - 1] = '\0';
        len--;
    }
}

// Copy the given string and normalize it (used for input URLs)
char *normalize_full_url(const char *input) {
    char *copy = strdup(input);
    if (!copy) {
        perror("strdup normalize_full_url");
        exit(EXIT_FAILURE);
    }
    normalize_inplace(copy);
    return copy;
}

// Check if full URL is a standard Wikipedia article (no Category:, File:, etc.)
int is_article_url(const char *full) {
    size_t prefix_len = strlen(WIKI_PREFIX);
    if (strncmp(full, WIKI_PREFIX, prefix_len) != 0) {
        return 0;
    }
    const char *title = full + prefix_len;
    if (*title == '\0') return 0;

    for (const char *p = title; *p; ++p) {
        if (*p == ':') {
            return 0;
        }
    }
    return 1;
}

// Turn an href from HTML into a normalized absolute Wikipedia URL, or NULL
char *normalize_wiki_href(const char *href) {
    char *full = NULL;

    // Relative link: /wiki/Some_Page
    if (strncmp(href, "/wiki/", 6) == 0) {
        size_t len = strlen("https://en.wikipedia.org") + strlen(href) + 1;
        full = malloc(len);
        if (!full) {
            perror("malloc normalize_wiki_href");
            return NULL;
        }
        strcpy(full, "https://en.wikipedia.org");
        strcat(full, href);
    }
    // Already a full Wikipedia article URL
    else if (strncmp(href, "https://en.wikipedia.org/wiki/", 30) == 0) {
        full = strdup(href);
        if (!full) {
            perror("strdup normalize_wiki_href");
            return NULL;
        }
    } else {
        return NULL; // Not a page we care about
    }

    normalize_inplace(full);
    if (!is_article_url(full)) {
        free(full);
        return NULL;
    }
    return full;
}

struct MemoryChunk {
    char *memory;
    size_t size;
};

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct MemoryChunk *mem = (struct MemoryChunk *)userp;

    char *ptr = realloc(mem->memory, mem->size + total + 1);
    if (!ptr) {
        fprintf(stderr, "realloc() failed in write_callback\n");
        return 0;
    }
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, total);
    mem->size += total;
    mem->memory[mem->size] = '\0';
    return total;
}

// Download HTML for given URL. Caller must free() the returned string.
char *fetch_page(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }

    struct MemoryChunk chunk;
    chunk.memory = NULL;
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "simple-mt-crawler/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed for %s: %s\n",
                url, curl_easy_strerror(res));
        if (chunk.memory) free(chunk.memory);
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_cleanup(curl);
    return chunk.memory; // may be NULL if page was empty
}

// Decide whether to add new_url as a child of current in the BFS
void maybe_add_link(Node *current, const char *new_url) {
    // Check depth limit first
    if (current->depth + 1 > max_depth) {
        return;
    }

    // Check visited list
    pthread_mutex_lock(&visited_lock);
    if (visited_contains_locked(new_url)) {
        pthread_mutex_unlock(&visited_lock);
        return;
    }

    // Not visited yet -> create child node and mark visited
    Node *child = create_node(new_url, current->depth + 1, current);
    visited_add_locked(child->url);
    pthread_mutex_unlock(&visited_lock);

    // Check if this child is the target page
    pthread_mutex_lock(&result_lock);
    if (!found && strcmp(child->url, target_url) == 0) {
        found = 1;
        found_node = child;
        pthread_mutex_unlock(&result_lock);
        return; // do not enqueue; we already have the path
    }
    pthread_mutex_unlock(&result_lock);

    // Not the target -> add to queue for further crawling
    enqueue_node(&url_queue, child);
}

// Scan HTML for <a href="..."> links and process Wikipedia article links
void parse_links(Node *current, const char *html) {
    const char *p = html;

    while ((p = strstr(p, "<a")) != NULL) {
        // Stop early if someone already found the target
        pthread_mutex_lock(&result_lock);
        int done = found;
        pthread_mutex_unlock(&result_lock);
        if (done) return;

        const char *href_attr = strstr(p, "href=\"");
        if (!href_attr) {
            p += 2; // move forward a bit
            continue;
        }
        href_attr += 6; // skip past href="

        const char *end_quote = strchr(href_attr, '"');
        if (!end_quote) {
            break;
        }

        size_t len = (size_t)(end_quote - href_attr);
        if (len == 0) {
            p = end_quote + 1;
            continue;
        }

        char *href_value = malloc(len + 1);
        if (!href_value) {
            perror("malloc href_value");
            return;
        }
        memcpy(href_value, href_attr, len);
        href_value[len] = '\0';

        // Try to turn it into a normalized Wikipedia article URL
        char *full_url = normalize_wiki_href(href_value);
        free(href_value);

        // If valid article link, attempt to add it to BFS
        if (full_url) {
            maybe_add_link(current, full_url);
            free(full_url);
        }

        p = end_quote + 1;
    }
}


void print_path(Node *node) {
    if (!node) return;

    int capacity = 16;
    int count = 0;
    Node **list = malloc(sizeof(Node *) * capacity);
    if (!list) {
        perror("malloc path list");
        return;
    }

    // Collect nodes from target back to start
    Node *cur = node;
    while (cur) {
        if (count >= capacity) {
            capacity *= 2;
            Node **tmp = realloc(list, sizeof(Node *) * capacity);
            if (!tmp) {
                perror("realloc path list");
                free(list);
                return;
            }
            list = tmp;
        }
        list[count++] = cur;
        cur = cur->parent;
    }

    printf("Path found! %d step(s):\n", count - 1);

    FILE *outfile = fopen("path_output.txt", "w");
    if (outfile) {
        fprintf(outfile, "Path found! %d step(s):\n", count - 1);
    } else {
        perror("fopen path_output.txt");
    }

    // Print from start to target, with step numbers and short titles
    int step = 0;
    for (int i = count - 1; i >= 0; --i) {
        const char *full = list[i]->url;

        // Get just the last part after /wiki/
        const char *title = strrchr(full, '/');
        title = title ? title + 1 : full;

        // Make a simple copy where '_' becomes ' '
        char pretty[512];
        size_t j = 0;
        for (size_t k = 0; title[k] && j < sizeof(pretty) - 1; ++k) {
            pretty[j++] = (title[k] == '_') ? ' ' : title[k];
        }
        pretty[j] = '\0';

        printf("%d. %s\n", step, pretty);   // step: 0 = start, last = target
        if (outfile) {
            fprintf(outfile, "%d. %s\n", step, pretty);
        }
        step++;
    }

    if (outfile) {
        fclose(outfile);
        printf("Full path written to path_output.txt\n");
    }

    free(list);
}

// Each worker repeatedly takes a URL from the queue, fetches it,
// parses links, and adds new URLs to the queue.
void *worker_thread(void *arg) {
    (void)arg;

    while (1) {
        // Stop if another thread already found the target
        pthread_mutex_lock(&result_lock);
        int done = found;
        pthread_mutex_unlock(&result_lock);
        if (done) break;

        // Get next page from queue
        Node *current = dequeue_node(&url_queue);
        if (!current) {
            break; // queue empty -> nothing left to do
        }



        // Download the HTML
        char *html = fetch_page(current->url);
        if (!html) {
            continue; // network error; skip this branch
        }

        // Parse links in this page
        parse_links(current, html);
        free(html);
    }

    return NULL;
}


// Read start URL, target URL, and depth from urls.txt
// File format (three lines):
//   line 1: start URL
//   line 2: target URL
//   line 3: depth (integer)
int read_urls_from_file(const char *filename,
                        char **start_out,
                        char **target_out,
                        int *depth_out) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen urls.txt");
        return -1;
    }

    char line[2048];

    // Read start URL (line 1)
    if (!fgets(line, sizeof(line), f)) {
        fprintf(stderr, "urls.txt: missing start URL on line 1\n");
        fclose(f);
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0'; 
    *start_out = normalize_full_url(line);

    // Read target URL (line 2)
    if (!fgets(line, sizeof(line), f)) {
        fprintf(stderr, "urls.txt: missing target URL on line 2\n");
        fclose(f);
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';
    *target_out = normalize_full_url(line);

    // Read depth (line 3)
    if (!fgets(line, sizeof(line), f)) {
        fprintf(stderr, "urls.txt: missing depth on line 3\n");
        fclose(f);
        return -1;
    }
    *depth_out = atoi(line);

    fclose(f);
    return 0;
}


static void print_usage(const char *prog) {
    printf("USAGE (command line): %s <url-1> <url-2> <depth>\n", prog);
    printf("OR: just run '%s' and put 3 lines in urls.txt:\n", prog);
    printf("    line 1: start Wikipedia URL\n");
    printf("    line 2: target Wikipedia URL\n");
    printf("    line 3: max depth (integer)\n");
}

int main(int argc, char *argv[]) {
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        print_usage(argv[0]);
        return 0;
    }

    // Case 1: read from urls.txt (no extra command-line arguments)
    if (argc == 1) {
        if (read_urls_from_file("urls.txt", &start_url, &target_url, &max_depth) != 0) {
            fprintf(stderr, "Failed to read URLs from urls.txt\n");
            return 1;
        }
    }
    // Case 2: read from command line (3 arguments)
    else if (argc == 4) {
        start_url  = normalize_full_url(argv[1]);
        target_url = normalize_full_url(argv[2]);
        max_depth  = atoi(argv[3]);
    }
    // Anything else -> print usage
    else {
        print_usage(argv[0]);
        return 1;
    }

    if (max_depth <= 0) {
        fprintf(stderr, "Depth must be a positive integer.\n");
        return 1;
    }

    if (strncmp(start_url, WIKI_PREFIX, strlen(WIKI_PREFIX)) != 0 ||
        strncmp(target_url, WIKI_PREFIX, strlen(WIKI_PREFIX)) != 0) {
        fprintf(stderr, "Both URLs must be Wikipedia article URLs starting with %s\n",
                WIKI_PREFIX);
        return 1;
    }

    printf("Finding path from %s to %s.\n", start_url, target_url);

    if (strcmp(start_url, target_url) == 0) {
        printf("%s\n", start_url);
        return 0;
    }

    // Initialize libcurl once before starting threads
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "curl_global_init() failed\n");
        return 1;
    }
    init_queue(&url_queue);

    pthread_mutex_lock(&visited_lock);
    Node *root = create_node(start_url, 0, NULL);
    visited_add_locked(root->url);
    pthread_mutex_unlock(&visited_lock);

    enqueue_node(&url_queue, root);

    // Create worker threads
    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    if (!threads) {
        perror("malloc threads");
        return 1;
    }

    for (int i = 0; i < num_threads; ++i) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    // Wait for all threads to finish
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    // Check if we found a path
    pthread_mutex_lock(&result_lock);
    int have_path = found;
    Node *path_end = found_node;
    pthread_mutex_unlock(&result_lock);

    if (have_path && path_end) {
        print_path(path_end);
    } else {
        printf("No path found from %s to %s.\n", start_url, target_url);
    }

    curl_global_cleanup();
    return 0;
}
