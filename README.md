# cs-project
Multithreaded Wikipedia crawler

what i did

i used the graph model 
- We treat Wikipedia as a graph:
- Each article = a node (Node struct).
- Each hyperlink from one article to another = a directed edge.

BFS
- BFS to find a path.
- BFS explores in layers:
  - Start page = depth 0
  - Pages reachable in 1 click = depth 1
  - Pages reachable in 2 clicks = depth 2, etc.
- BFS guarantees that if we find the target, the path we find is the one with the fewest clicks.

multithreaded BFS
- Instead of one thread doing BFS, we use multiple worker threads.
- All workers share:
  - A thread-safe queue of nodes (url_queue).
  - A visited list of URLs so we don’t repeat work.
- and then repeat
  - take URL from the queue
  - fetch it using liburl
  - Parses <a href="..."> links.
  - and adds new Eikipedia article URLs back into BFS
 
  Error handling (total of 25 (if you really want to count it in numbers))
  First main error handling: Input and usange errors *
If user calls with wrong arguments:
- print_usage() and exit.
  - read_urls_from_file:
- If urls.txt cannot be opened: perror("fopen urls.txt"), return -1.
  - If any of the three lines are missing: print specific error (e.g. “missing start URL on line 1”) and return -1.
- If max_depth <= 0: print "Depth must be a positive integer." and exit.
- If start_url or target_url do not start with https://en.wikipedia.org/wiki/:
  - Print error and exit.
 
Second main error handling: Memoery Allocation error
- when using malloc and strdup for nodes if fail call perror(...) and exit(EXIT_FAILURE)

Third main error handling: libcurl/ network errors 
- In fetch_page:
    - If curl_easy_init fails: print error and return NULL.
    - If curl_easy_perform fails: print error message including URL and libcurl error string, free memory, return NULL.
- In worker_thread:
    - If fetch_page returns NULL, the thread just continues, skipping that page but continuing the overall search.

 Fourth main error handling: Thread and global init errors 
- If curl_global_init fails then it will print "curl_global_init() failed" and exit.
- If pthread_create fails for any worker then it will print error via perror("pthread_create") and exit.
- If allocating the threads array fails then it will print error and exit.

Fifth main error handling: Path output file errors
- In print_path, when opening path_output.txt:

'FILE *outfile = fopen("path_output.txt", "w");
if (!outfile) {
    perror("fopen path_output.txt");
}'

- If the file can’t be opened, the program still prints the path to the terminal; only file output is skipped.

Sixth main error handling: No path found
- after all threads finish, if found is 0 then print "No path found from <start> to <target>."





