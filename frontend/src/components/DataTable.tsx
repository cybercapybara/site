import type { ReactNode } from 'react';

import { apiErrorMessage } from '@/lib/api/client';

/**
 * Dumb, presentational table. No sorting/filtering engine — the page
 * owns data fetching (usePagedQuery) and any bespoke filters above it.
 * DataTable only renders columns, loading/error/empty states, and dims
 * the body while a paged query shows placeholder (previous-page) data.
 */
export interface Column<Row> {
  header: ReactNode;
  /** Cell renderer for a row. */
  cell: (row: Row) => ReactNode;
  /** Extra classes for both the <th> and the <td>. */
  className?: string;
}

interface DataTableProps<Row> {
  columns: Column<Row>[];
  rows: Row[] | undefined;
  rowKey: (row: Row) => string | number;
  isLoading?: boolean;
  error?: unknown;
  emptyText?: string;
  /** Dim the body while showing previous-page placeholder data. */
  isPlaceholder?: boolean;
  /** Per-row props (e.g. onClick / className) for selectable tables. */
  rowProps?: (row: Row) => React.HTMLAttributes<HTMLTableRowElement>;
}

export function DataTable<Row>({
  columns,
  rows,
  rowKey,
  isLoading,
  error,
  emptyText = 'Nothing here yet.',
  isPlaceholder,
  rowProps,
}: DataTableProps<Row>) {
  if (isLoading) return <p className="text-muted-foreground">Loading…</p>;
  if (error) return <p className="text-destructive">{apiErrorMessage(error, 'Failed to load.')}</p>;
  if (!rows) return null;
  if (rows.length === 0) return <p className="text-muted-foreground">{emptyText}</p>;

  return (
    <table className={`w-full text-sm ${isPlaceholder ? 'opacity-50' : ''}`}>
      <thead>
        <tr className="border-b text-left text-muted-foreground">
          {columns.map((c, i) => (
            <th key={i} className={`py-2 pr-4 ${c.className ?? ''}`}>
              {c.header}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row) => {
          const extra = rowProps?.(row);
          const { className: extraClass, ...restProps } = extra ?? {};
          return (
            <tr
              key={rowKey(row)}
              className={`border-b last:border-0 ${extraClass ?? ''}`}
              {...restProps}
            >
              {columns.map((c, i) => (
                <td key={i} className={`py-2 pr-4 ${c.className ?? ''}`}>
                  {c.cell(row)}
                </td>
              ))}
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}
