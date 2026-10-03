#ifndef LIBRARY_BOOK_H
#define LIBRARY_BOOK_H

#include <string>
#include <iostream>

// ---------------------------------------------------------------------------
// Book - represents a single title in the library catalogue.
//
// OOP notes for viva:
//   * Encapsulation: all fields are private, exposed via getters/setters.
//   * Abstraction:  the user only sees display()/serialize()/deserialize().
//   * Constructor validates mandatory fields (non-empty title).
// ---------------------------------------------------------------------------
class Book {
public:
    Book() = default;

    explicit Book(int id,
                  std::string title,
                  std::string author,
                  std::string category,
                  std::string isbn,
                  std::string publication,
                  int totalCopies,
                  int availableCopies);

    // ---- accessors (const-correct) --------------------------------------
    int          getId()            const noexcept { return id_; }
    const std::string& getTitle()   const noexcept { return title_; }
    const std::string& getAuthor()  const noexcept { return author_; }
    const std::string& getCategory()const noexcept { return category_; }
    const std::string& getIsbn()    const noexcept { return isbn_; }
    const std::string& getPublication() const noexcept { return publication_; }
    int          getTotalCopies()   const noexcept { return totalCopies_; }
    int          getAvailableCopies()const noexcept { return availableCopies_; }

    // ---- mutators (with validation) -------------------------------------
    void setTitle(const std::string& title);
    void setAuthor(const std::string& author);
    void setCategory(const std::string& category);
    void setIsbn(const std::string& isbn);
    void setPublication(const std::string& publication);
    void setTotalCopies(int total);
    void setAvailableCopies(int available);

    // ---- behaviour ------------------------------------------------------
    void display(std::ostream& os = std::cout) const;
    bool isValid() const noexcept;                 // basic sanity check
    bool hasAvailableCopies() const noexcept { return availableCopies_ > 0; }

    // ---- serialization (binary) ----------------------------------------
    // Returns the byte size written so callers can stream records easily.
    std::size_t serialize(std::ostream& os) const;
    bool        deserialize(std::istream& is);

private:
    int         id_             = 0;
    std::string title_;
    std::string author_;
    std::string category_;
    std::string isbn_;
    std::string publication_;
    int         totalCopies_     = 0;
    int         availableCopies_ = 0;
};

#endif // LIBRARY_BOOK_H