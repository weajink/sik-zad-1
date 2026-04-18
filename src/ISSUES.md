Correctness issues (address first)                                                           
                                                   
  - Server recvfrom buffer is too small. run_server_loop reads into char                       
  buffer[CLIENT_MESSAGE_SIZE] (12 bytes). A client sending 13+ bytes gets silently truncated by
   UDP, and the server cannot detect that the message was malformed-overlong. Read into a      
  larger buffer (e.g. 65507) and let your parser reject the length (kayles_server.h:157).      
  Related: CLIENT_MESSAGE_SIZE = MSG_TYPE+PLAYER_ID+GAME_ID+PAWN+2 conflates the max valid     
  client message (10 bytes) with the MSG_WRONG_MSG echo size (12 bytes). That +2 is a hidden
  magic number that will rot. Name the two quantities separately.
  - address_str.data() passed to getaddrinfo (kayles_common.h:112). string_view::data() is not
  guaranteed null-terminated. It happens to work because you feed it optarg, but the function's
   signature lies about what it accepts. Take const char* or const std::string&.
  - games.rbegin() assumes monotonic game_id (kayles_game.h:211). Once next_game_id (a         
  uint32_t) wraps, "last-created" ≠ highest key, and emplace will collide silently on an       
  existing id. Track the pending-join game explicitly in an std::optional<uint32_t>.
  - Port 0 for client — parse_port is shared, but the spec says the server allows 0 ("any      
  port") and the client requires 1+. The client currently accepts 0 (kayles_client.cpp:32).    
  - Uninitialized game_state_t on the client (kayles_client.cpp:142). game_state_t state; then
  memcpy(&state, buffer, recv_bytes) leaves trailing bytes indeterminate. Spec guarantees the  
  bitmap is sized to cover iteration, so it's currently not observable, but state{} costs
  nothing.                                                                                     
  - STATUS_OFFSET = PLAYER_ID_SIZE + PLAYER_ID_SIZE + GAME_ID_SIZE (kayles_client.cpp:129).
  Numerically correct, but names the fields in the wrong order vs. the struct layout.          
  Future-you will read this and doubt it.
  - check_timeouts silently mutates status on timeout but never resets last_move_time          
  (kayles_game.h:142). Combined with the post-finish retention rule, a game that timed out in  
  TURN_A can then be retained based on whichever last_move_time is more recent. Worth a comment
   or — better — a clearer two-method split: tick() returning whether the game is collectible. 
                  
  Design / architecture

  - Duplicated state between KaylesServer and KaylesGameMap. Both own server_timeout, max_pawn,
   and row (kayles_server.h:78–98). Pick one owner. Right now they can drift.
  - Packed struct as wire format. __attribute__((packed)) plus sendto(&state, ...) is the      
  classic "works on my compiler" pattern. Taking the address of an unaligned member inside a   
  packed struct is formally UB, and you rely on compiler-specific extensions. Write
  serialize(const GameState&, std::span<std::byte>) and deserialize(...) functions. Also       
  removes the fragile sizeof(game_state_t) - MAX_BITMAP_SIZE + ... math in run_server_loop.
  - pawn_row_t = std::vector<bool>. vector<bool> is infamous; here you only index it, so it
  works — but since the bitmap is also the wire representation, a single std::array<uint8_t,   
  32> wrapper that serves both roles would eliminate the for (i=0..max_pawn) bit |= 
  (1<<(7-i%8)) conversion in get_game_state.                                                   
  - Exceptions for startup errors. start() throws runtime_error, but main doesn't catch — so
  std::terminate gives the user a not-great message and a non-spec-compliant exit. Either catch
   in main and return 1, or return an error type.
  - No SIGINT handling. run() is an infinite loop; there's no way to shut down cleanly. Fine   
  for grading, but worth a volatile sig_atomic_t flag.                                         
  - Parser's error_index semantics. When a message is too short, you return len (one past the
  end). When a value is bad, you return offset (the start of the bad field). The spec says     
  error_index points to "a byte the server cannot interpret" — which is fine for offset, but
  len points to a byte that doesn't exist. Worth a deliberate decision + comment.              
                  
  C++ / readability                                                                            
  
  - using namespace in headers (kayles_server.h:16–17, kayles_game.h:16, kayles_client.h:14).  
  This is the canonical "never do this" of C++. Every TU that includes these now has
  kayles_common::* leaked into its global namespace.                                           
  - Everything is header-only. No templates, no reason. Split into .h / .cpp. Right now
  parse_client_message (90 lines), run_server_loop, get_message_from_buffer are all implicitly 
  inline in every TU that sees them. Compile times and link behavior both suffer.
  - #include <kayles_common.h>. Use quotes for project headers; angle brackets are             
  conventionally for system/third-party.                                                       
  - Naming inconsistency. game_state_t (snake+_t) vs ClientMessage (Pascal) vs WrongMessage.
  Also, _t is reserved by POSIX — don't prefix your own types with it.                         
  - Stray }; closing namespaces (kayles_common.h:141, kayles_game.h:274, kayles_server.h:208).
  Should be }. A junior tell.                                                                  
  - static char buffer[...] inside run_server_loop. The static is load-bearing only if you call
   the function recursively or across threads — neither applies. Drop it and the memset;       
  declare it fresh each call.
  - Inconsistent error reporting. std::cerr << "cannot create a socket" vs "bind failed" vs    
  "Server successfully started..." — mix of capitalization, no strerror(errno) on most         
  syscalls, no consistent prefix. Pick a format ("kayles_server: bind: <strerror>\n") and apply
   it everywhere.                                                                              
  - assert(false) in KaylesGame::move (kayles_game.h:115). Under NDEBUG this is a fall-through.
   Use std::unreachable() like you do in the server's switch.                                  
  - Comments like // 1. get message type are fine, but the function is doing enough repetitive
  work that a small helper (read_u32_be, read_u8) would halve the code and eliminate the       
  hand-written offset tracking.
                                                                                               
  What I'd say to the junior                                                                   
  
  The logic is basically right, the structure is readable, and you reached for std::expected   
  and std::unreachable in appropriate places — good. The problems are the ones that come from
  working alone without review: headers doing too much (including using namespace), a          
  packed-struct wire format that sidesteps rather than solves the serialization problem,
  duplicated state between KaylesServer and KaylesGameMap, and a few real bugs (truncated UDP
  reads, rbegin() after wraparound, string_view::data() to a C API). Fix the bugs first, then
  split headers from implementations, then replace the packed struct with explicit
  serialization — each change makes the next one easier.